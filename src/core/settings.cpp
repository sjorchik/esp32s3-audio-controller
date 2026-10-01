// Реалізація SettingsStore: один blob у NVS з версією формату, кеш у RAM,
// відкладений запис з дебаунсом у власній легкій задачі.

#include "core/settings.h"

#include <Preferences.h>
#include <string.h>
#include <type_traits>

#include "audio/audio_processor.h"
#include "config/audio_config.h"
#include "config/defaults.h"
#include "config/display_config.h"
#include "config/settings_config.h"

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
static_assert(static_cast<uint8_t>(AudioProcType::Tda7318) == 0 &&
                  static_cast<uint8_t>(AudioProcType::Pt2313l) == 1,
              "Settings::processorType comment assumes Tda7318=0, Pt2313l=1");
static_assert(sizeof(StoredBlob) <= 1024, "keep the blob small");

constexpr uint8_t kProcTypeMax = static_cast<uint8_t>(AudioProcType::Pt2313l);

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
    // Для першого запуску беремо поточний тип зі стенду (audio_cfg::kTestProcType),
    // щоб замінити жорстку константу в main.cpp без зміни поведінки.
    s.processorType = static_cast<uint8_t>(audio_cfg::kTestProcType);
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        snprintf(s.inputNames[i], sizeof(s.inputNames[i]), "%s", defaults::kInputNames[i]);
    }
    s.brightness = defaults::kDefaultBrightness;
    s.displayFlipped = display_cfg::kDefaultFlipped;
    s.bass = defaults::kDefaultBass;
    s.treble = defaults::kDefaultTreble;
    s.balance = defaults::kDefaultBalance;
    s.loudness = defaults::kDefaultLoudness;
    s.lastInput = defaults::kDefaultInput;
    s.lastStation = 0;
    s.lastVolume = defaults::kDefaultVolume;
    s.lastMute = defaults::kDefaultMute;
}

// bool з некоректним бітовим патерном — UB при читанні, тому читаємо як байт.
void normalizeBool(bool& b) {
    uint8_t raw;
    memcpy(&raw, &b, 1);
    b = (raw != 0);
}

// Перевірка даних, прочитаних з NVS (захист від пошкодженого, але формально
// валідного blob-а). Діапазони тембру/гучності тут НЕ перевіряються: їх обрізає
// драйвер за capabilities() конкретного чіпа.
void sanitize(Settings& s) {
    if (s.processorType > kProcTypeMax) {
        s.processorType = static_cast<uint8_t>(audio_cfg::kTestProcType);
    }
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        s.inputNames[i][sizeof(s.inputNames[i]) - 1] = '\0';
    }
    if (s.lastInput >= defaults::kInputCount) {
        s.lastInput = defaults::kDefaultInput;
    }
    normalizeBool(s.displayFlipped);
    normalizeBool(s.loudness);
    normalizeBool(s.lastMute);
}

// ---------------------------------------------------------------------------
// Внутрішні операції (мʼютекс уже захоплено)
// ---------------------------------------------------------------------------
void markDirtyLocked() {
    const uint32_t now = millis();
    if (!s_dirty) {
        s_dirty = true;
        s_firstDirtyMs = now;
    }
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

// Виконується в фоновій задачі.
void flushIfDue() {
    Lock lock;
    if (!lock.ok() || !s_dirty) {
        return;
    }
    const uint32_t now = millis();
    const bool quiet = (now - s_lastRequestMs) >= settings_cfg::kSaveDebounceMs;
    const bool tooOld = (now - s_firstDirtyMs) >= settings_cfg::kSaveMaxDelayMs;
    if (!quiet && !tooOld) {
        return;
    }
    if (!writeLocked()) {
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
        } else if (len != sizeof(StoredBlob)) {
            Serial.printf("[SET] stored size %u != %u, using defaults\n",
                          static_cast<unsigned>(len), static_cast<unsigned>(sizeof(StoredBlob)));
        } else {
            StoredBlob blob;
            const size_t got = prefs.getBytes(settings_cfg::kNvsBlobKey, &blob, sizeof(blob));
            if (got != sizeof(blob)) {
                Serial.println("[SET] stored read failed, using defaults");
            } else if (blob.version != settings_cfg::kFormatVersion ||
                       blob.payloadSize != sizeof(Settings)) {
                Serial.printf("[SET] stored format v%u/%u != v%u/%u, using defaults\n",
                              static_cast<unsigned>(blob.version),
                              static_cast<unsigned>(blob.payloadSize),
                              static_cast<unsigned>(settings_cfg::kFormatVersion),
                              static_cast<unsigned>(sizeof(Settings)));
            } else {
                memcpy(&loaded, &blob.data, sizeof(Settings));
                haveStored = true;
            }
        }
        prefs.end();
    } else {
        Serial.println("[SET] NVS open failed, using defaults");
        s_nvsOk = false;
    }

    if (haveStored) {
        sanitize(loaded);
        s_settings = loaded;
        s_dirty = false;
        Serial.println("[SET] loaded from NVS");
        return true;
    }

    // Перший запуск / несумісний формат: дефолти й одразу в NVS.
    fillDefaults(s_settings);
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
    if (!s_dirty) {
        return true;
    }
    if (!writeLocked()) {
        markDirtyLocked();
        return false;
    }
    return true;
}

bool SettingsStore::isDirty() {
    return s_dirty;
}

bool SettingsStore::resetToDefaults() {
    Lock lock;
    if (!lock.ok()) {
        return false;
    }
    fillDefaults(s_settings);
    s_dirty = false;
    if (!writeLocked()) {
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
    prefs.end();
    s_dirty = false;  // щоб відкладений запис не воскресив ключ
    return true;
}

uint32_t SettingsStore::writeCount() {
    return s_writeCount;
}
