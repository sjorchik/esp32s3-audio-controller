// Реалізація SettingsStore: один blob у NVS з версією формату, кеш у RAM,
// відкладений запис з дебаунсом у власній легкій задачі.
// [USB14] Розкладка після кроку USB14: profiles[4] (4-й слот — профіль «Player»), + playerShuffle / playerRepeat: Settings 138 Б,
// blob 142 Б. БЕЗ МІГРАЦІЇ: blob v8 старої довжини (132 Б) не збігається з sizeof(StoredBlob) і відхиляється з логом (діють
// дефолти, одразу записуються). Номер версії (settings_cfg::kFormatVersion, config/settings_config.h) цим кроком НЕ змінено
// (файл не прикріплено): відрізняє формати довжина + payloadSize; бажано підняти його до 9 окремою правкою. Станції (LittleFS),
// Wi-Fi та мапа IR цим не чіпаються: load() / resetToDefaults() / eraseStored() торкаються лише ключа blob-а й ключів позиції
// плеєра (`pl_usb_*`). Позиція плеєра зберігається ОКРЕМО (кеш у RAM, ключі `pl_usb_dir` / `pl_usb_trk` / `pl_usb_pos`) і пишеться
// тією ж фоновою задачею з тим самим дебаунсом, що й blob (нова фонова задача не створюється).
// [V1] Формат v8: профіль входу = {volume, eqGainsDb[5], balanceDb, vuSensDb} = 8 Б; Settings 128 Б, blob 132 Б.
// БЕЗ МІГРАЦІЇ з v7 (прошивка на СТЕРТИЙ flash): blob v7 (130 Б) відхиляється з логом, діють дефолти.
// [Prompt 48a] (v7) Формат v7: 3 логічні входи (0 = радіо, 1 = TV Box, 2 = Computer), профіль = {volume,
// eqGainsDb[5], balanceDb}, без типу аудіопроцесора (чипи вирізано).
// [Prompt 46b] (v6: 5 логічних входів.)
// МІГРАЦІЙ НЕМАЄ (рішення власника: прошивка на стертий flash): blob із будь-якою іншою версією, довжиною
// чи payload-ом ігнорується з логом, і діють значення за замовчуванням (одразу записуються в NVS).

#include "core/settings.h"

#include <Preferences.h>
#include <stddef.h>
#include <string.h>
#include <type_traits>

#include "audio/eq.h"
#include "config/balance_config.h"
#include "config/defaults.h"
#include "config/display_config.h"
#include "config/eq_config.h"
#include "config/settings_config.h"
#include "config/vu_sens_config.h"  // [V1]

namespace {

// ---------------------------------------------------------------------------
// Формат у NVS
// ---------------------------------------------------------------------------
struct StoredBlob {
    uint8_t version;       // settings_cfg::kFormatVersion
    uint8_t reserved;      // 0
    uint16_t payloadSize;  // sizeof(Settings) на момент запису
    Settings data;
};

static_assert(std::is_trivially_copyable<Settings>::value,
              "Settings is stored as raw bytes");
static_assert(sizeof(Settings::inputNames) / sizeof(Settings::inputNames[0]) ==
                  defaults::kInputCount,
              "Settings::inputNames must have defaults::kInputCount rows");
static_assert(sizeof(StoredBlob) <= 1024, "keep the blob small");

// ---------------------------------------------------------------------------
// Перевірки розкладки (формат v8)
// ---------------------------------------------------------------------------
static_assert(defaults::kInputCount == 3, "format v8 is laid out for 3 logical inputs (radio + TV Box + Computer)");
static_assert(sizeof(InputProfile) == 1 + eq_cfg::kBandCount + 1 + 1,
              "InputProfile = volume + EQ bands + balanceDb + vuSensDb, no padding");
static_assert(sizeof(Settings::profiles) / sizeof(Settings::profiles[0]) == player_mode_cfg::kProfileCount,
              "[USB14] profiles[]: one per logical input + the Player slot");
static_assert(sizeof(Settings) == 138, "[USB14] Settings is 138 bytes (was 128: +1 Player profile (8) + playerShuffle + playerRepeat)");
static_assert(sizeof(StoredBlob) == 142, "[USB14] blob is 142 bytes (was 132; differs from every earlier format: 130/132/144/164/168/170/202)");
static_assert(defaults::kInputCount >= 1, "profiles[0] (radio) must exist");


// ---------------------------------------------------------------------------
// Стан модуля
// ---------------------------------------------------------------------------
Settings s_settings;  // статична памʼять → нулі до begin()
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
bool s_nvsOk = false;
bool s_dirty = false;
uint32_t s_firstDirtyMs = 0;
uint32_t s_lastRequestMs = 0;
uint32_t s_writeCount = 0;

// [USB14] Позиція плеєра (джерело USB), окремо від blob-а. Усе під мʼютексом.
char s_resDir[player_mode_cfg::kResumePathMax] = {0};
char s_resTrack[player_mode_cfg::kResumePathMax] = {0};
uint32_t s_resPos = 0;
bool s_resValid = false;
bool s_resDirty = false;
uint32_t s_resWrites = 0;

// RAII-захоплення рекурсивного мʼютекса. До begin() (мʼютекса ще немає)
// працює без блокування — це однопотокова рання фаза старту.
class Lock {
public:
    Lock() {
        if (s_mutex == nullptr) {
            ok_ = true;
            return;
        }
        held_ = (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(settings_cfg::kMutexTimeoutMs)) ==
                 pdTRUE);
        ok_ = held_;
    }
    ~Lock() {
        if (held_) {
            xSemaphoreGiveRecursive(s_mutex);
        }
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    bool ok() const { return ok_; }

private:
    bool held_ = false;
    bool ok_ = false;
};

// ---------------------------------------------------------------------------
// Значення за замовчуванням і перевірка
// ---------------------------------------------------------------------------
void fillDefaults(Settings& s) {
    memset(&s, 0, sizeof(s));
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        snprintf(s.inputNames[i], sizeof(s.inputNames[i]), "%s", defaults::kInputNames[i]);
    }
    s.brightness = defaults::kDefaultBrightness;
    s.displayFlipped = display_cfg::kDefaultFlipped;
    s.lastInput = defaults::kDefaultInput;
    s.lastStation = 0;
    s.lastMute = defaults::kDefaultMute;
    // [Prompt 46a] Профіль кожного входу: типова гучність, еквалайзер плоский (0 дБ), [P48a] баланс 0 дБ.
    // [USB14] І профіль «Player» (слот kInputCount): ті самі значення. Обґрунтування: файл звучить через DSP радіо, рівень виходу
    // файлу = типовий рівень станції (file_player_cfg::kFileLevelDb), тож і гучність / EQ / чутливість VU за замовчуванням —
    // як у радіо; перше прослуховування — на низькій гучності.
    for (uint8_t i = 0; i < player_mode_cfg::kProfileCount; ++i) {
        InputProfile& p = s.profiles[i];
        p.volume = defaults::kDefaultVolume;
        for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
            p.eqGainsDb[b] = 0;
        }
        p.balanceDb = balance_cfg::kDefaultDb;
        p.vuSensDb = vu_sens_cfg::kDefaultDb;  // [V1]
    }
    // [Prompt 48a] Усі три входи дозволені (0x07).
    s.inputEnabledMask = settings_cfg::kInputMaskDefault;
    s.playerShuffle = false;  // [USB14]
    s.playerRepeat = false;
}

// bool з некоректним бітовим патерном — UB при читанні, тому читаємо як байт.
void normalizeBool(bool& b) {
    uint8_t raw;
    memcpy(&raw, &b, 1);
    b = (raw != 0);
}

// Перевірка даних, прочитаних з NVS (захист від пошкодженого, але формально
// валідного blob-а). Діапазон гучності тут НЕ перевіряється: його обрізає AppController
// (audio_cfg::kVolumeUiMin..kVolumeUiMax). Пресети EQ обрізаються тут (eq_cfg::kGainMinDb..kGainMaxDb);
// [P48a] balanceDb поза діапазоном (balance_cfg::kMinDb..kMaxDb) -> 0; [V1] vuSensDb поза діапазоном
// (vu_sens_cfg::kMinDb..kMaxDb) -> 0.
void sanitize(Settings& s) {
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        s.inputNames[i][sizeof(s.inputNames[i]) - 1] = '\0';
    }
    if (s.lastInput >= defaults::kInputCount) {
        s.lastInput = defaults::kDefaultInput;
    }
    normalizeBool(s.displayFlipped);
    normalizeBool(s.lastMute);
    normalizeBool(s.playerShuffle);  // [USB14]
    normalizeBool(s.playerRepeat);
    // [Prompt 30/46a] Пошкоджений пресет еквалайзера (будь-якого входу) не повинен потрапити в DSP.
    // [USB14] І профіль «Player» (слот kInputCount).
    for (uint8_t i = 0; i < player_mode_cfg::kProfileCount; ++i) {
        for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
            int8_t& g = s.profiles[i].eqGainsDb[b];
            if (g < eq_cfg::kGainMinDb) g = eq_cfg::kGainMinDb;
            if (g > eq_cfg::kGainMaxDb) g = eq_cfg::kGainMaxDb;
        }
        s.profiles[i].balanceDb = balance_cfg::sanitizeDb(s.profiles[i].balanceDb);  // [Prompt 48a]
        s.profiles[i].vuSensDb = vu_sens_cfg::sanitizeDb(s.profiles[i].vuSensDb);    // [V1] поза діапазоном -> 0
    }
    // [Prompt 39] Маска входів: зайві біти скидаємо, радіо (біт 0) завжди дозволене.
    s.inputEnabledMask = settings_cfg::normalizeInputMask(s.inputEnabledMask);
}

// [Prompt 30/46a] Передає пресет ВХОДУ 0 (радіо) в екземпляр еквалайзера радіо. Пресети інших входів
// сюди не потрапляють (їх застосує P47 / line_path). Безпечно з будь-якої задачі й до старту плеєра.
void applyEqLocked() {
    eq::setAllDb(s_settings.profiles[0].eqGainsDb);
}

// ---------------------------------------------------------------------------
// Внутрішні операції (мʼютекс уже захоплено)
// ---------------------------------------------------------------------------
void markDirtyLocked() {
    const uint32_t now = millis();
    if (!s_dirty && !s_resDirty) {  // [USB14] таймери спільні для blob-а й позиції плеєра
        s_firstDirtyMs = now;
    }
    s_dirty = true;
    s_lastRequestMs = now;
}

// [USB14] Те саме для позиції плеєра: той самий дебаунс (settings_cfg::kSaveDebounceMs / kSaveMaxDelayMs).
void markResumeDirtyLocked() {
    const uint32_t now = millis();
    if (!s_dirty && !s_resDirty) {
        s_firstDirtyMs = now;
    }
    s_resDirty = true;
    s_lastRequestMs = now;
}

bool writeLocked() {
    StoredBlob blob;
    memset(&blob, 0, sizeof(blob));  // детермінований вміст, включно з padding
    blob.version = settings_cfg::kFormatVersion;
    blob.payloadSize = static_cast<uint16_t>(sizeof(Settings));
    memcpy(&blob.data, &s_settings, sizeof(Settings));

    Preferences prefs;
    if (!prefs.begin(settings_cfg::kNvsNamespace, false)) {
        Serial.println("[SET] write failed: NVS open");
        return false;
    }
    const size_t written = prefs.putBytes(settings_cfg::kNvsBlobKey, &blob, sizeof(blob));
    prefs.end();

    if (written != sizeof(blob)) {
        Serial.printf("[SET] write failed: %u of %u bytes\n", static_cast<unsigned>(written),
                      static_cast<unsigned>(sizeof(blob)));
        return false;
    }
    s_dirty = false;
    ++s_writeCount;
    return true;
}

// [USB14] Запис позиції плеєра в NVS (мʼютекс захоплено). Немає збереженої позиції -> ключі видаляються. Рядки пишуться
// безпосередньо зі статичних буферів (без копій на стеку: стек фонової задачі малий).
bool writeResumeLocked() {
    Preferences prefs;
    if (!prefs.begin(settings_cfg::kNvsNamespace, false)) {
        Serial.println("[SET] resume write failed: NVS open");
        return false;
    }
    bool ok = true;
    if (s_resValid) {
        ok = prefs.putString(player_mode_cfg::kNvsKeyUsbDir, s_resDir) > 0;
        ok = (prefs.putString(player_mode_cfg::kNvsKeyUsbTrack, s_resTrack) > 0) && ok;
        ok = (prefs.putUInt(player_mode_cfg::kNvsKeyUsbPos, s_resPos) == sizeof(uint32_t)) && ok;
    } else {
        prefs.remove(player_mode_cfg::kNvsKeyUsbDir);
        prefs.remove(player_mode_cfg::kNvsKeyUsbTrack);
        prefs.remove(player_mode_cfg::kNvsKeyUsbPos);
    }
    prefs.end();
    if (!ok) {
        Serial.println("[SET] resume write failed");
        return false;
    }
    s_resDirty = false;
    ++s_resWrites;
    return true;
}

// [USB14] Читання позиції плеєра з NVS (мʼютекс захоплено; викликається з load()).
void loadResumeLocked() {
    s_resValid = false;
    s_resDir[0] = '\0';
    s_resTrack[0] = '\0';
    s_resPos = 0;
    Preferences prefs;
    if (!prefs.begin(settings_cfg::kNvsNamespace, true)) {
        return;
    }
    if (prefs.isKey(player_mode_cfg::kNvsKeyUsbDir) && prefs.isKey(player_mode_cfg::kNvsKeyUsbTrack)) {
        const size_t d = prefs.getString(player_mode_cfg::kNvsKeyUsbDir, s_resDir, sizeof(s_resDir));
        const size_t t = prefs.getString(player_mode_cfg::kNvsKeyUsbTrack, s_resTrack, sizeof(s_resTrack));
        s_resPos = prefs.getUInt(player_mode_cfg::kNvsKeyUsbPos, 0);
        s_resDir[sizeof(s_resDir) - 1] = '\0';
        s_resTrack[sizeof(s_resTrack) - 1] = '\0';
        s_resValid = (d > 0 && t > 0 && s_resDir[0] == '/');
        if (s_resPos > 24u * 3600u) {  // пошкоджене значення: позиція з початку
            s_resPos = 0;
        }
    }
    prefs.end();
    if (s_resValid) {
        Serial.printf("[SET] player resume: dir='%s' track='%s' pos=%lu s\n", s_resDir, s_resTrack,
                      static_cast<unsigned long>(s_resPos));
    } else {
        s_resDir[0] = '\0';
        s_resTrack[0] = '\0';
        s_resPos = 0;
    }
}

// Виконується в фоновій задачі.
void flushIfDue() {
    Lock lock;
    if (!lock.ok() || (!s_dirty && !s_resDirty)) {
        return;
    }
    const uint32_t now = millis();
    const bool quiet = (now - s_lastRequestMs) >= settings_cfg::kSaveDebounceMs;
    const bool tooOld = (now - s_firstDirtyMs) >= settings_cfg::kSaveMaxDelayMs;
    if (!quiet && !tooOld) {
        return;
    }
    bool ok = true;
    if (s_dirty && !writeLocked()) {
        ok = false;
    }
    if (s_resDirty && !writeResumeLocked()) {  // [USB14]
        ok = false;
    }
    if (!ok) {
        // Повторимо через kSaveDebounceMs, а не кожні kFlushPollMs.
        s_firstDirtyMs = now;
        s_lastRequestMs = now;
    }
}

void flushTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(settings_cfg::kFlushPollMs));
        flushIfDue();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Публічне API
// ---------------------------------------------------------------------------
bool SettingsStore::begin() {
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_mutex == nullptr) {
            Serial.println("[SET] mutex create failed");
            fillDefaults(s_settings);
            return false;
        }
    }

    {
        Lock lock;
        fillDefaults(s_settings);  // get() до load() повертає дефолти, не сміття
        // Відкриття на запис створює namespace, якщо його ще немає (щоб
        // read-only відкриття в load() не падало на першому запуску).
        Preferences prefs;
        s_nvsOk = prefs.begin(settings_cfg::kNvsNamespace, false);
        if (s_nvsOk) {
            prefs.end();
        } else {
            Serial.println("[SET] NVS open failed");
        }
    }

    if (s_task == nullptr) {
        const BaseType_t ok = xTaskCreatePinnedToCore(
            flushTask, "settings_flush", settings_cfg::kFlushTaskStackBytes, nullptr,
            settings_cfg::kFlushTaskPriority, &s_task, settings_cfg::kFlushTaskCore);
        if (ok != pdPASS) {
            s_task = nullptr;
            Serial.println("[SET] flush task create failed (requestSave will not write)");
            return false;
        }
    }
    return s_nvsOk;
}

bool SettingsStore::load() {
    Lock lock;
    if (!lock.ok()) {
        return false;
    }

    Settings loaded;
    bool haveStored = false;

    Preferences prefs;
    if (prefs.begin(settings_cfg::kNvsNamespace, true)) {
        const size_t len = prefs.getBytesLength(settings_cfg::kNvsBlobKey);
        if (len == 0) {
            Serial.println("[SET] no stored settings (first run), using defaults");
        } else if (len < offsetof(StoredBlob, data) || len > sizeof(StoredBlob)) {
            Serial.printf("[SET] stored size %u unsupported, using defaults\n",
                          static_cast<unsigned>(len));
        } else {
            // [Prompt 48a / V1] Приймаємо ЛИШЕ поточний формат v8 (версія + довжина + payload). Будь-який інший
            // blob (старі версії v1..v7, чужа довжина) не мігрується: значення за замовчуванням.
            alignas(4) uint8_t raw[sizeof(StoredBlob)];
            memset(raw, 0, sizeof(raw));
            const size_t got = prefs.getBytes(settings_cfg::kNvsBlobKey, raw, len);
            uint16_t payload = 0;
            memcpy(&payload, raw + offsetof(StoredBlob, payloadSize), sizeof(payload));
            const uint8_t version = raw[offsetof(StoredBlob, version)];

            if (got != len) {
                Serial.println("[SET] stored read failed, using defaults");
            } else if (version == settings_cfg::kFormatVersion && len == sizeof(StoredBlob) &&
                       payload == sizeof(Settings)) {
                memcpy(&loaded, raw + offsetof(StoredBlob, data), sizeof(Settings));
                haveStored = true;
            } else {
                Serial.printf("[SET] stored format v%u/%u (len %u) unsupported (no migration), using defaults\n",
                              static_cast<unsigned>(version), static_cast<unsigned>(payload),
                              static_cast<unsigned>(len));
            }
        }
        prefs.end();
    } else {
        Serial.println("[SET] NVS open failed, using defaults");
        s_nvsOk = false;
    }

    loadResumeLocked();  // [USB14] позиція плеєра (окремі ключі; не залежить від версії blob-а)

    if (haveStored) {
        sanitize(loaded);
        s_settings = loaded;
        s_dirty = false;
        applyEqLocked();  // [Prompt 30/46a] пресет входу 0 -> екземпляр радіо
        Serial.println("[SET] loaded from NVS");
        return true;
    }

    // Перший запуск / несумісний формат: дефолти й одразу в NVS.
    fillDefaults(s_settings);
    applyEqLocked();  // [Prompt 30]
    if (!writeLocked()) {
        markDirtyLocked();  // фонова задача спробує ще раз
        return false;
    }
    Serial.println("[SET] defaults saved");
    return true;
}

bool SettingsStore::save() {
    Lock lock;
    if (!lock.ok()) {
        return false;
    }
    if (!writeLocked()) {
        markDirtyLocked();
        return false;
    }
    return true;
}

Settings& SettingsStore::get() {
    return s_settings;
}

Settings SettingsStore::snapshot() {
    Lock lock;  // якщо не вдалося захопити — віддаємо копію без захисту
    Settings copy;
    memcpy(&copy, &s_settings, sizeof(Settings));
    return copy;
}

void SettingsStore::modify(void (*fn)(Settings&, void*), void* ctx) {
    if (fn == nullptr) {
        return;
    }
    Lock lock;
    if (!lock.ok()) {
        return;
    }
    fn(s_settings, ctx);
    markDirtyLocked();
}

void SettingsStore::requestSave() {
    Lock lock;
    if (lock.ok()) {
        markDirtyLocked();
    }
}

bool SettingsStore::flush() {
    Lock lock;
    if (!lock.ok()) {
        return false;
    }
    if (!s_dirty && !s_resDirty) {
        return true;
    }
    bool ok = true;
    if (s_dirty && !writeLocked()) {
        markDirtyLocked();
        ok = false;
    }
    if (s_resDirty && !writeResumeLocked()) {  // [USB14]
        markResumeDirtyLocked();
        ok = false;
    }
    return ok;
}

bool SettingsStore::isDirty() {
    return s_dirty || s_resDirty;  // [USB14]
}

bool SettingsStore::resetToDefaults() {
    Lock lock;
    if (!lock.ok()) {
        return false;
    }
    fillDefaults(s_settings);
    applyEqLocked();  // [Prompt 30/46a] скидання налаштувань = плоский еквалайзер (вхід 0) і в DSP радіо
    s_dirty = false;
    // [USB14] Скидання налаштувань забуває й позицію плеєра (ключі `pl_usb_*` видаляються).
    s_resValid = false;
    s_resDir[0] = '\0';
    s_resTrack[0] = '\0';
    s_resPos = 0;
    s_resDirty = false;
    bool ok = writeLocked();
    if (!writeResumeLocked()) {
        markResumeDirtyLocked();
        ok = false;
    }
    if (!ok) {
        markDirtyLocked();
        return false;
    }
    return true;
}

bool SettingsStore::eraseStored() {
    Lock lock;
    if (!lock.ok()) {
        return false;
    }
    Preferences prefs;
    if (!prefs.begin(settings_cfg::kNvsNamespace, false)) {
        return false;
    }
    prefs.remove(settings_cfg::kNvsBlobKey);  // інші ключі (мапа IR) не чіпаємо
    prefs.remove(player_mode_cfg::kNvsKeyUsbDir);    // [USB14] позиція плеєра теж
    prefs.remove(player_mode_cfg::kNvsKeyUsbTrack);
    prefs.remove(player_mode_cfg::kNvsKeyUsbPos);
    prefs.end();
    s_dirty = false;  // щоб відкладений запис не воскресив ключ
    s_resDirty = false;
    return true;
}

uint32_t SettingsStore::writeCount() {
    return s_writeCount;
}

// --- [USB14] Позиція плеєра -----------------------------------------------------------------------------------------
bool SettingsStore::getPlayerResume(PlayerSource src, char* dirOut, size_t dirCap, char* trackOut, size_t trackCap,
                                    PlayerResume* info) {
    if (src != PlayerSource::Usb || dirOut == nullptr || trackOut == nullptr || dirCap == 0 || trackCap == 0) {
        return false;
    }
    Lock lock;
    if (!lock.ok() || !s_resValid) {
        return false;
    }
    if (strlen(s_resDir) >= dirCap || strlen(s_resTrack) >= trackCap) {
        return false;
    }
    strcpy(dirOut, s_resDir);
    strcpy(trackOut, s_resTrack);
    if (info != nullptr) {
        info->valid = true;
        info->posSec = s_resPos;
    }
    return true;
}

void SettingsStore::setPlayerResume(PlayerSource src, const char* dir, const char* track, uint32_t posSec) {
    if (src != PlayerSource::Usb || dir == nullptr || track == nullptr) {
        return;
    }
    const size_t dl = strlen(dir);
    const size_t tl = strlen(track);
    if (dl == 0 || tl == 0 || dir[0] != '/' || dl >= sizeof(s_resDir) || tl >= sizeof(s_resTrack)) {
        return;  // нечинне значення не записуємо
    }
    Lock lock;
    if (!lock.ok()) {
        return;
    }
    if (s_resValid && s_resPos == posSec && strcmp(s_resDir, dir) == 0 && strcmp(s_resTrack, track) == 0) {
        return;  // без змін: NVS не чіпаємо
    }
    memcpy(s_resDir, dir, dl + 1);
    memcpy(s_resTrack, track, tl + 1);
    s_resPos = posSec;
    s_resValid = true;
    markResumeDirtyLocked();
}

void SettingsStore::clearPlayerResume(PlayerSource src) {
    if (src != PlayerSource::Usb) {
        return;
    }
    Lock lock;
    if (!lock.ok() || !s_resValid) {
        return;
    }
    s_resValid = false;
    s_resDir[0] = '\0';
    s_resTrack[0] = '\0';
    s_resPos = 0;
    markResumeDirtyLocked();
}

uint32_t SettingsStore::playerResumeWriteCount() {
    return s_resWrites;
}

