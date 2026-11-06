// [USB10] Черга відтворення: автоперехід, послідовний режим / перемішування, повтор, пропуск пошкоджених файлів.
// Див. usb_queue.h (правила), config/usb_queue_config.h. Уся логіка черги виконується в задачі usb_nav (usb_browser.cpp);
// публічні функції лише ставлять команди й читають опублікований знімок стану.
//
// Автоперехід: кожні kPollMs задача перевіряє AudioPlayer::takeFileEnded() (прапорець піднімається ПІСЛЯ kEndTailMs, мʼюту ЦАП
// і Idle — USB7), далі вибирає наступний трек і викликає AudioPlayer::playFile(). Час переходу друкується в логу
// («[USBQ] track ... transition: flag->accept=… accept->active=… accept->playing=… flag->playing=… ms»).

#include "usb/usb_queue.h"

#if FEATURE_USB_MODE

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "audio/audio_player.h"
#include "config/player_mode_config.h"  // [USB14]
#include "config/usb_queue_config.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_browser.h"
#include "usb/usb_mode.h"
#include "usb/usb_msc_mount.h"
#include "usb/usb_nav_internal.h"
#include "core/settings.h"  // [USB14] SettingsStore::get/setPlayerResume

static_assert(player_mode_cfg::kResumePathMax >= usb_browser_cfg::kPathMax,
              "[USB14] resume path buffers must hold any browser path");

namespace usb_queue {

namespace {

constexpr const char* kTag = "[USBQ]";

enum class Phase : uint8_t { Inactive, Starting, Running };

// --- Дані черги (лише задача usb_nav; опублікований знімок — під мьютексом usb_nav::lock) ---------------------------------
usb_nav::Listing s_list;          // знімок файлів папки (лише файли, порядок списку браузера)
uint16_t* s_order = nullptr;      // перестановка 0..count-1 (PSRAM, kMaxEntries)
uint16_t s_orderLen = 0;
int32_t s_pos = -1;               // позиція в порядку проходу
int32_t s_cur = -1;               // поточний трек (номер файлу в s_list)
uint16_t s_hist[usb_queue_cfg::kHistoryMax];
uint8_t s_histLen = 0;
uint16_t s_fwd[usb_queue_cfg::kHistoryMax];
uint8_t s_fwdLen = 0;

Phase s_phase = Phase::Inactive;
uint8_t s_skips = 0;
const char* s_reason = nullptr;
uint32_t s_started = 0;
uint32_t s_qver = 0;
bool s_appliedShuffle = false;
bool s_haveList = false;

uint32_t s_tFlag = 0;        // AudioPlayer::fileEndedAtMs() попереднього треку (0 — перший трек / ручний перехід)
uint32_t s_tAccept = 0;
uint32_t s_tActive = 0;
uint32_t s_idleSince = 0;
uint32_t s_lastPathCheck = 0;
bool s_sawLeave = false;   // у Starting плеєр хоч раз був НЕ в Playing (підхопив запит)
bool s_skipBack = false;   // пропуск биттого треку йде назад (після prev)

std::atomic<bool> s_shuffle{false};
std::atomic<bool> s_repeat{false};

// Опублікований знімок (під usb_nav::lock).
Status s_pub = {};
char s_pubPath[usb_browser_cfg::kPathMax] = {0};

// --- [USB14] Пауза, підготовлена черга, відновлення, знімок позиції -----------------------------------------------------------
// Буфери шляхів — у PSRAM (стек usb_nav малий, внутрішня RAM дорога): виділяються в onWorkerStart(), вивільняються в onWorkerStop().
struct Scratch {
    char snapDir[player_mode_cfg::kResumePathMax];    // знімок: папка поточного треку (під usb_nav::lock)
    char snapTrack[player_mode_cfg::kResumePathMax];  // знімок: імʼя поточного треку (під usb_nav::lock)
    char rsDir[player_mode_cfg::kResumePathMax];      // збережена папка (відновлення)
    char rsTrack[player_mode_cfg::kResumePathMax];    // збережений трек (відновлення)
};
Scratch* s_sc = nullptr;
bool s_snapValid = false;                 // під usb_nav::lock
std::atomic<uint32_t> s_snapPos{0};       // остання відома позиція треку, с (оновлює лише usb_nav; читає commitResume)
uint32_t s_snapLastMs = 0;
bool s_prevPaused = false;
std::atomic<bool> s_prepared{false};      // черга Idle, але є підготовлений трек (publish())
std::atomic<bool> s_startReq{false};      // OK на підготовленій черзі (requestPlayPause -> tick)
int32_t s_prepPos = -1;                   // позиція старту підготовленого треку, с (-1 — з початку); лише usb_nav
uint32_t s_resumeCommits = 0;             // скільки разів черга передала позицію в SettingsStore за сеанс (діагностика)

enum class Rs : uint8_t { Idle, WaitRoot, WaitGoto, WaitGotoRoot };
Rs s_rs = Rs::Idle;                       // автомат відновлення (лише usb_nav)
std::atomic<bool> s_rsRequested{false};   // requestResume() -> tick
uint32_t s_rsT0 = 0;
uint32_t s_rsReqId = 0;
uint32_t s_rsPos = 0;
char s_rsTarget[usb_browser_cfg::kPathMax] = {0};  // куди чекаємо потрапити (для перевірки cwd)
const char* s_rsOutcome = nullptr;        // підсумок останнього відновлення (статичний рядок; діагностика)

inline uint32_t rnd(uint32_t n) { return n == 0 ? 0 : esp_random() % n; }  // зсув ≤ n / 2^32 ≈ 2e-7 при n ≤ 1000: прийнятний

bool buildPath(uint16_t t, char* out, size_t cap) {
    if (t >= s_list.count) {
        return false;
    }
    const char* dir = s_list.cwd;
    const bool root = (dir[0] == '/' && dir[1] == '\0');
    const int n = snprintf(out, cap, "%s%s%s", dir, root ? "" : "/", usb_nav::nameAt(s_list, t));
    return n > 0 && static_cast<size_t>(n) < cap;
}

void publish() {
    Status st = {};
    st.state = (s_phase == Phase::Inactive) ? State::Idle : (s_phase == Phase::Starting ? State::Starting : State::Playing);
    st.count = s_haveList ? s_list.count : 0;
    st.index = (s_phase == Phase::Inactive) ? -1 : s_cur;
    st.position = (s_phase == Phase::Inactive) ? -1 : s_pos;
    st.skips = s_skips;
    st.started = s_started;
    st.reason = s_reason;
    st.paused = false;
    st.prepared = false;
    s_prepared.store(s_phase == Phase::Inactive && s_haveList && s_list.count > 0 && s_cur >= 0);
    char path[usb_browser_cfg::kPathMax];
    path[0] = '\0';
    if (s_phase != Phase::Inactive && s_cur >= 0) {
        buildPath(static_cast<uint16_t>(s_cur), path, sizeof(path));
    }
    usb_nav::lock(UINT32_MAX);
    st.version = ++s_qver;
    s_pub = st;
    strcpy(s_pubPath, path);
    usb_nav::unlock();
}

void clearHistory() {
    s_histLen = 0;
    s_fwdLen = 0;
}

void pushHist(uint16_t t) {
    if (s_histLen >= usb_queue_cfg::kHistoryMax) {
        memmove(s_hist, s_hist + 1, (usb_queue_cfg::kHistoryMax - 1) * sizeof(uint16_t));
        --s_histLen;
    }
    s_hist[s_histLen++] = t;
}

void pushFwd(uint16_t t) {
    if (s_fwdLen >= usb_queue_cfg::kHistoryMax) {
        memmove(s_fwd, s_fwd + 1, (usb_queue_cfg::kHistoryMax - 1) * sizeof(uint16_t));
        --s_fwdLen;
    }
    s_fwd[s_fwdLen++] = t;
}

void logOrder(const char* why) {
    char buf[usb_queue_cfg::kOrderLogMax * 5 + 16];
    size_t off = 0;
    buf[0] = '\0';
    const uint16_t lim = s_orderLen < usb_queue_cfg::kOrderLogMax ? s_orderLen : usb_queue_cfg::kOrderLogMax;
    for (uint16_t i = 0; i < lim && off + 6 < sizeof(buf); ++i) {
        off += static_cast<size_t>(snprintf(buf + off, sizeof(buf) - off, "%u ", static_cast<unsigned>(s_order[i])));
    }
    Serial.printf("%s order (%s, %s, %u tracks%s): %s\n", kTag, why, s_shuffle.load() ? "shuffle" : "sequential",
                  static_cast<unsigned>(s_orderLen), s_orderLen > lim ? ", first shown" : "", buf);
}

// Будує порядок проходу. first >= 0 — цей трек ставиться першим (поточний); first < 0 — новий прохід (перший ≠ lastPlayed).
void buildOrder(int32_t first, const char* why) {
    const uint16_t n = s_list.count;
    s_orderLen = n;
    for (uint16_t i = 0; i < n; ++i) {
        s_order[i] = i;
    }
    if (!s_shuffle.load()) {
        s_pos = (first >= 0 && first < n) ? first : 0;
        if (n > 0 && why != nullptr) {
            logOrder(why);
        }
        return;
    }
    if (first >= 0 && first < n) {
        const uint16_t t = s_order[0];
        s_order[0] = s_order[first];
        s_order[first] = t;
        for (int32_t i = static_cast<int32_t>(n) - 1; i >= 2; --i) {  // Fisher–Yates над [1..n-1]
            const uint32_t j = 1 + rnd(static_cast<uint32_t>(i));
            const uint16_t x = s_order[i];
            s_order[i] = s_order[j];
            s_order[j] = x;
        }
    } else {
        for (int32_t i = static_cast<int32_t>(n) - 1; i >= 1; --i) {  // Fisher–Yates над [0..n-1]
            const uint32_t j = rnd(static_cast<uint32_t>(i) + 1);
            const uint16_t x = s_order[i];
            s_order[i] = s_order[j];
            s_order[j] = x;
        }
        if (n > 1 && s_cur >= 0 && s_order[0] == static_cast<uint16_t>(s_cur)) {  // не починати прохід щойно зіграним
            const uint32_t j = 1 + rnd(static_cast<uint32_t>(n) - 1);
            const uint16_t x = s_order[0];
            s_order[0] = s_order[j];
            s_order[j] = x;
        }
    }
    s_pos = 0;
    if (n > 0 && why != nullptr) {
        logOrder(why);
    }
}

bool mounted() { return usb_msc_mount::isMounted(); }

// [USB14] Знімок «поточний трек + позиція» (RAM; публікується під usb_nav::lock для commitResume() з інших задач). Шляхи
// беруться з s_list (папка черги, а не поточна папка браузера).
void setSnapshotTrack(uint16_t t, uint32_t pos) {
    if (s_sc == nullptr || t >= s_list.count) {
        return;
    }
    const char* name = usb_nav::nameAt(s_list, t);
    const char* dir = s_list.cwd;
    if (strlen(name) >= player_mode_cfg::kResumePathMax || strlen(dir) >= player_mode_cfg::kResumePathMax) {
        return;
    }
    usb_nav::lock(UINT32_MAX);
    strcpy(s_sc->snapDir, dir);
    strcpy(s_sc->snapTrack, name);
    s_snapValid = true;
    usb_nav::unlock();
    s_snapPos.store(pos);
    s_snapLastMs = millis();
}

// Передає знімок у SettingsStore (кеш у RAM; запис у NVS — відкладений фоновою задачею, і лише якщо значення змінилось).
// Викликається з usb_nav у ЧОТИРЬОХ випадках: старт треку, пауза, кінець черги, вихід із режиму (onWorkerStop).
void commitSnapshot(const char* why) {
    if (s_sc == nullptr || !s_snapValid) {
        return;
    }
    SettingsStore::setPlayerResume(PlayerSource::Usb, s_sc->snapDir, s_sc->snapTrack, s_snapPos.load());
    ++s_resumeCommits;
    Serial.printf("%s resume commit (%s): '%s' / '%s' @ %lu s\n", kTag, why, s_sc->snapDir, s_sc->snapTrack,
                  static_cast<unsigned long>(s_snapPos.load()));
}

// Позиція поточного треку з плеєра -> знімок (без запису в Settings).
void refreshSnapshotPos() {
    if (s_snapValid && AudioPlayer::fileActive()) {
        s_snapPos.store(AudioPlayer::filePositionSec());
    }
    s_snapLastMs = millis();
}

void stopQueue(const char* reason, bool stopPlayer) {
    if (stopPlayer && AudioPlayer::fileActive()) {
        AudioPlayer::stop();
    }
    const bool was = (s_phase != Phase::Inactive);
    s_phase = Phase::Inactive;
    s_reason = reason;
    s_prevPaused = false;
    if (was && strcmp(reason, "end_of_queue") == 0) {
        // [USB14] Усю чергу програно: наступний вхід у режим / OK почне останній трек ЗНОВУ (а не з кінця).
        s_snapPos.store(0);
        s_prepPos = -1;
        commitSnapshot("end_of_queue");
    }
    if (was) {
        Serial.printf("%s queue stopped: %s (started=%u tracks)\n", kTag, reason, static_cast<unsigned>(s_started));
    }
    publish();
}

// Запит файлу t (startSec >= 0 — абсолютна секунда старту, -1 — з початку). false — не прийнято (*why = причина).
bool startTrack(uint16_t t, const char** why, int32_t startSec = -1) {
    char path[usb_browser_cfg::kPathMax];
    if (!buildPath(t, path, sizeof(path))) {
        *why = "path_too_long";
        return false;
    }
    if (!usb_nav::modeOk()) {
        *why = "mode_not_ready";
        return false;
    }
    const bool ok = AudioPlayer::playFile(path, startSec);
    if (!usb_nav::modeOk()) {  // вихід із режиму почався між перевіркою й запитом: гасимо файл (end() чекає Idle)
        AudioPlayer::stop();
        *why = "mode_not_ready";
        return false;
    }
    if (!ok) {
        *why = "play_refused";
        return false;
    }
    s_cur = t;
    s_phase = Phase::Starting;
    s_tAccept = millis();
    s_tActive = 0;
    s_idleSince = 0;
    s_sawLeave = false;
    s_lastPathCheck = s_tAccept;
    s_prevPaused = false;  // [USB14] новий файл грає без паузи
    s_prepPos = -1;
    Serial.printf("%s track pos=%d/%u idx=%u: '%s'%s\n", kTag, static_cast<int>(s_pos + 1), static_cast<unsigned>(s_orderLen),
                  static_cast<unsigned>(t), path, startSec > 0 ? " (resume position)" : "");
    setSnapshotTrack(t, startSec > 0 ? static_cast<uint32_t>(startSec) : 0u);  // [USB14]
    commitSnapshot("track");
    publish();
    return true;
}

// Вибір наступного треку за правилами. pushCur — запамʼятати поточний у історії (перемішування). false — кінець черги.
bool pickNext(bool pushCur, uint16_t* t) {
    const uint16_t n = s_list.count;
    if (n == 0) {
        return false;
    }
    const bool shuf = s_shuffle.load();
    if (shuf && s_fwdLen > 0) {  // повернення на трек, покинутий через prev
        if (pushCur && s_cur >= 0) {
            pushHist(static_cast<uint16_t>(s_cur));
        }
        *t = s_fwd[--s_fwdLen];
        return true;
    }
    if (s_pos + 1 < static_cast<int32_t>(s_orderLen)) {
        if (shuf && pushCur && s_cur >= 0) {
            pushHist(static_cast<uint16_t>(s_cur));
        }
        ++s_pos;
        *t = s_order[s_pos];
        return true;
    }
    if (!s_repeat.load()) {
        return false;
    }
    if (shuf && pushCur && s_cur >= 0) {
        pushHist(static_cast<uint16_t>(s_cur));
    }
    buildOrder(-1, "new pass");
    *t = s_order[0];
    return true;
}

bool pickPrev(uint16_t* t) {
    const uint16_t n = s_list.count;
    if (n == 0) {
        return false;
    }
    if (s_shuffle.load()) {
        if (s_histLen > 0) {
            if (s_cur >= 0) {
                pushFwd(static_cast<uint16_t>(s_cur));
            }
            *t = s_hist[--s_histLen];
        } else {
            *t = static_cast<uint16_t>(s_cur >= 0 ? s_cur : 0);  // немає історії: поточний з початку
        }
        return true;
    }
    if (s_pos > 0) {
        --s_pos;
    } else if (s_repeat.load()) {
        s_pos = static_cast<int32_t>(s_orderLen) - 1;
    }  // інакше лишаємось на першому: той самий трек з початку
    *t = s_order[s_pos];
    return true;
}

// Пропуск треку, що не відкрився / збій читання: переходимо далі, не більше kMaxConsecutiveSkips поспіль.
void failAndSkip(const char* why) {
    for (;;) {
        if (!mounted()) {
            stopQueue("media_removed", true);
            return;
        }
        ++s_skips;
        Serial.printf("%s SKIP %u/%u: track idx=%d failed (%s; player=%s err=%s)\n", kTag, static_cast<unsigned>(s_skips),
                      static_cast<unsigned>(usb_queue_cfg::kMaxConsecutiveSkips), static_cast<int>(s_cur), why,
                      AudioPlayer::stateName(AudioPlayer::state()),
                      AudioPlayer::lastFileError() != nullptr ? AudioPlayer::lastFileError() : "-");
        if (s_skips > usb_queue_cfg::kMaxConsecutiveSkips) {
            stopQueue("too_many_skips", true);
            return;
        }
        uint16_t t = 0;
        if (s_skipBack) {
            if (!s_shuffle.load() && s_pos <= 0 && !s_repeat.load()) {
                stopQueue("skipped_to_start", true);
                return;
            }
            pickPrev(&t);
        } else if (!pickNext(false, &t)) {
            stopQueue("skipped_to_end", true);
            return;
        }
        const char* w = nullptr;
        if (startTrack(t, &w)) {
            return;
        }
        if (strcmp(w, "mode_not_ready") == 0 || strcmp(w, "path_too_long") == 0) {
            stopQueue(w, true);
            return;
        }
        why = w;  // play_refused: рахується як збій наступного треку
        s_cur = t;
    }
}

// Природний кінець треку: автоперехід.
void onEnded() {
    s_skips = 0;
    s_skipBack = false;
    uint16_t t = 0;
    if (!pickNext(true, &t)) {
        stopQueue("end_of_queue", false);
        return;
    }
    const char* w = nullptr;
    if (!startTrack(t, &w)) {
        if (strcmp(w, "mode_not_ready") == 0 || strcmp(w, "path_too_long") == 0) {
            stopQueue(w, true);
            return;
        }
        s_cur = t;
        failAndSkip(w);
    }
}

// --- [USB14] Відновлення при вході в режим (автомат у задачі usb_nav; див. usb_queue.h: requestResume) ------------------------
void rsEnd(const char* outcome) {
    s_rs = Rs::Idle;
    s_rsOutcome = outcome;
    Serial.printf("%s resume: %s\n", kTag, outcome);
}

// Готує чергу з файлів поточної папки браузера (ординал треку — номер ФАЙЛУ); автостарт — за kAutoResumeOnEnter.
void rsPrepare(uint16_t ordinal, uint32_t pos) {
    const usb_nav::Listing& lv = usb_nav::live();
    if (!usb_nav::copyFiles(lv, s_list)) {
        rsEnd("queue_overflow - nothing restored");
        return;
    }
    s_skipBack = false;
    s_haveList = true;
    s_appliedShuffle = s_shuffle.load();
    clearHistory();
    s_skips = 0;
    s_reason = nullptr;
    s_cur = -1;
    s_started = 0;
    s_tFlag = 0;
    buildOrder(ordinal, "resume");
    s_cur = ordinal;
    const int32_t startSec = pos > 0 ? static_cast<int32_t>(pos) : -1;
    if (player_mode_cfg::kAutoResumeOnEnter) {
        const char* w = nullptr;
        if (!startTrack(ordinal, &w, startSec)) {
            s_cur = ordinal;
            s_phase = Phase::Inactive;
            s_reason = w;
            s_prepPos = startSec;
            publish();
            Serial.printf("%s resume: autostart refused (%s) - track prepared, OK starts it\n", kTag, w != nullptr ? w : "?");
            s_rs = Rs::Idle;
            s_rsOutcome = "autostart_refused";
            return;
        }
        rsEnd("autostart: track started from the saved position");
    } else {
        s_phase = Phase::Inactive;
        s_prepPos = startSec;
        setSnapshotTrack(ordinal, pos);
        publish();
        rsEnd("prepared (kAutoResumeOnEnter = false) - OK starts the track from the saved position");
    }
}

// Пошук збереженого треку у ПОТОЧНІЙ папці браузера (після переходу в збережену папку).
void rsFind() {
    const usb_nav::Listing& lv = usb_nav::live();
    for (uint16_t i = lv.dirs; i < lv.count; ++i) {
        if (strcmp(usb_nav::nameAt(lv, i), s_sc->rsTrack) == 0) {
            rsPrepare(static_cast<uint16_t>(i - lv.dirs), s_rsPos);
            return;
        }
    }
    if (strcmp(lv.cwd, "/") == 0) {
        rsEnd("track gone - staying in root, no autostart");
        return;
    }
    const char* why = nullptr;
    uint32_t id = 0;
    if (usb_browser::requestGoto("/", &id, &why)) {
        s_rsReqId = id;
        strcpy(s_rsTarget, "/");
        s_rs = Rs::WaitGotoRoot;
        s_rsT0 = millis();
        Serial.printf("%s resume: track '%s' is gone from '%s' - returning to root\n", kTag, s_sc->rsTrack, lv.cwd);
    } else {
        rsEnd("track gone - cannot return to root (stays in the folder), no autostart");
    }
}

void resumeTick(uint32_t now) {
    if (s_rs == Rs::Idle) {
        if (!s_rsRequested.exchange(false)) {
            return;
        }
        s_rs = Rs::WaitRoot;
        s_rsT0 = now;
        s_rsOutcome = nullptr;
        Serial.printf("%s resume: requested (autostart=%d)\n", kTag, player_mode_cfg::kAutoResumeOnEnter ? 1 : 0);
    }
    if (!usb_nav::modeOk()) {
        s_rs = Rs::Idle;
        return;
    }
    if (s_phase != Phase::Inactive) {
        rsEnd("skipped - queue already started");
        return;
    }
    const usb_nav::Listing& lv = usb_nav::live();
    switch (s_rs) {
        case Rs::WaitRoot: {
            if (!(lv.valid && lv.depth == 0 && mounted())) {
                if (static_cast<uint32_t>(now - s_rsT0) >= player_mode_cfg::kResumeWaitRootMs) {
                    rsEnd("root list timeout - nothing restored");
                }
                return;
            }
            PlayerResume info = {};
            if (!SettingsStore::getPlayerResume(PlayerSource::Usb, s_sc->rsDir, sizeof(s_sc->rsDir), s_sc->rsTrack,
                                                sizeof(s_sc->rsTrack), &info)) {
                rsEnd("nothing saved - staying in root, no autostart");
                return;
            }
            s_rsPos = info.posSec;
            Serial.printf("%s resume: saved '%s' / '%s' @ %lu s\n", kTag, s_sc->rsDir, s_sc->rsTrack,
                          static_cast<unsigned long>(s_rsPos));
            if (strcmp(s_sc->rsDir, "/") == 0) {
                rsFind();
                return;
            }
            const char* why = nullptr;
            uint32_t id = 0;
            if (!usb_browser::requestGoto(s_sc->rsDir, &id, &why)) {
                Serial.printf("%s resume: goto refused (%s)\n", kTag, why != nullptr ? why : "?");
                rsEnd("saved folder cannot be opened - staying in root, no autostart");
                return;
            }
            s_rsReqId = id;
            strncpy(s_rsTarget, s_sc->rsDir, sizeof(s_rsTarget) - 1);
            s_rsTarget[sizeof(s_rsTarget) - 1] = '\0';
            s_rs = Rs::WaitGoto;
            s_rsT0 = now;
            return;
        }
        case Rs::WaitGoto:
        case Rs::WaitGotoRoot: {
            usb_browser::Status st;
            if (usb_browser::status(&st) && st.lastReqId >= s_rsReqId) {
                const bool inTarget = lv.valid && strcmp(lv.cwd, s_rsTarget) == 0;
                if (s_rs == Rs::WaitGotoRoot) {
                    rsEnd(inTarget ? "track gone - back in root, no autostart" : "track gone - root unavailable");
                    return;
                }
                if (!inTarget) {
                    Serial.printf("%s resume: goto '%s' failed (%s)\n", kTag, s_rsTarget,
                                  st.lastWhy != nullptr ? st.lastWhy : "?");
                    rsEnd("folder gone - staying in root, no autostart");
                    return;
                }
                rsFind();
                return;
            }
            if (static_cast<uint32_t>(now - s_rsT0) >= player_mode_cfg::kResumeStepTimeoutMs) {
                rsEnd("goto timeout - nothing restored");
            }
            return;
        }
        case Rs::Idle:
            return;
    }
}

// OK на підготовленій черзі: запустити підготовлений трек (зі збереженої позиції після відновлення, інакше з початку).
void startPrepared() {
    if (s_phase != Phase::Inactive || !s_haveList || s_cur < 0 || s_list.count == 0) {
        Serial.printf("%s start: nothing prepared\n", kTag);
        return;
    }
    const uint16_t t = static_cast<uint16_t>(s_cur);
    const int32_t at = s_prepPos;
    s_skips = 0;
    s_skipBack = false;
    s_reason = nullptr;
    s_started = 0;
    s_tFlag = 0;
    const char* w = nullptr;
    if (!startTrack(t, &w, at)) {
        s_cur = t;
        failAndSkip(w);
    }
}

void rebuildForShuffle() {
    if (!s_haveList || s_list.count == 0 || s_order == nullptr) {
        return;
    }
    clearHistory();
    buildOrder(s_cur >= 0 ? s_cur : -1, s_shuffle.load() ? "shuffle on" : "shuffle off");
    Serial.printf("%s shuffle %s: current track kept, order rebuilt\n", kTag, s_shuffle.load() ? "ON" : "OFF");
    publish();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Хуки задачі usb_nav
// ---------------------------------------------------------------------------------------------------------------------
}  // namespace usb_queue

namespace usb_nav {
namespace q {

bool onWorkerStart() {
    using namespace usb_queue;
    s_phase = Phase::Inactive;
    s_reason = nullptr;
    s_haveList = false;
    s_cur = -1;
    s_pos = -1;
    s_orderLen = 0;
    s_skips = 0;
    s_started = 0;
    clearHistory();
    if (!allocListing(s_list)) {
        return false;
    }
    s_order = static_cast<uint16_t*>(
        heap_caps_malloc(sizeof(uint16_t) * usb_browser_cfg::kMaxEntries, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_order == nullptr) {
        freeListing(s_list);
        return false;
    }
    s_sc = static_cast<Scratch*>(heap_caps_malloc(sizeof(Scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));  // [USB14]
    if (s_sc == nullptr) {
        heap_caps_free(s_order);
        s_order = nullptr;
        freeListing(s_list);
        return false;
    }
    memset(s_sc, 0, sizeof(Scratch));
    s_snapValid = false;
    s_snapPos.store(0);
    s_prevPaused = false;
    s_prepPos = -1;
    s_startReq.store(false);
    s_rs = Rs::Idle;  // s_rsRequested НЕ чіпаємо: requestResume() виставляє його ДО створення задачі
    s_resumeCommits = 0;
    s_appliedShuffle = s_shuffle.load();
    publish();
    return true;
}

void onWorkerStop() {
    using namespace usb_queue;
    if (s_phase != Phase::Inactive) {
        // Вихід із режиму: файл зупиняє usb_mode (runExit); тут лише знімаємо стан черги.
        s_phase = Phase::Inactive;
        s_reason = "mode_exit";
        Serial.printf("%s queue stopped: mode_exit\n", kTag);
    }
    commitSnapshot("mode_exit");  // [USB14] останній трек / позиція (якщо є знімок); без змін — NVS не чіпається
    s_rs = Rs::Idle;
    s_rsRequested.store(false);
    s_startReq.store(false);
    s_haveList = false;
    s_cur = -1;
    s_pos = -1;
    s_orderLen = 0;
    publish();
    if (s_order != nullptr) {
        heap_caps_free(s_order);
        s_order = nullptr;
    }
    usb_nav::lock(UINT32_MAX);  // [USB14] commitResume() з іншої задачі читає s_sc під цим мьютексом
    s_snapValid = false;
    if (s_sc != nullptr) {
        heap_caps_free(s_sc);
        s_sc = nullptr;
    }
    usb_nav::unlock();
    freeListing(s_list);
    Serial.printf("%s worker stack min free=%u B\n", kTag, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void onMediaGone() {
    using namespace usb_queue;
    if (s_phase != Phase::Inactive) {
        stopQueue("media_removed", true);
    }
    s_haveList = false;
    resetListing(s_list);
    s_orderLen = 0;
    s_pos = -1;
    s_cur = -1;
    s_prevPaused = false;
    s_prepPos = -1;
    s_startReq.store(false);
    if (s_rs != Rs::Idle) {  // [USB14] носій змінився посеред відновлення: відновлення скасовано (корінь перечитає mediaTick)
        rsEnd("media changed - nothing restored");
    }
    clearHistory();
    publish();
}

void handle(const Cmd& c) {
    using namespace usb_queue;
    switch (c.type) {
        case CmdType::QPlay: {
            const Listing& lv = live();
            const char* why = nullptr;
            if (!lv.valid) {
                why = "list_invalid";
            } else if (c.expectVersion != 0 && c.expectVersion != lv.version) {
                why = "stale";
            } else if (c.index >= lv.count) {
                why = "bad_index";
            } else if (lv.e[c.index].isDir) {
                why = "not_file";
            }
            if (why != nullptr) {
                Serial.printf("%s play: refused (%s)\n", kTag, why);
                finish(c.reqId, false, why);
                break;
            }
            const uint16_t ordinal = static_cast<uint16_t>(c.index - lv.dirs);  // папки першими: номер файлу = індекс − dirs
            if (!copyFiles(lv, s_list)) {
                Serial.printf("%s play: refused (queue_overflow)\n", kTag);
                finish(c.reqId, false, "queue_overflow");
                break;
            }
            s_skipBack = false;
            s_haveList = true;
            s_appliedShuffle = s_shuffle.load();
            clearHistory();
            s_skips = 0;
            s_reason = nullptr;
            s_cur = -1;
            s_started = 0;
            s_tFlag = 0;
            buildOrder(ordinal, "play");
            const char* w = nullptr;
            if (!startTrack(ordinal, &w)) {
                s_phase = Phase::Inactive;
                s_reason = w;
                Serial.printf("%s play: refused (%s)\n", kTag, w);
                publish();
                finish(c.reqId, false, w);
                break;
            }
            Serial.printf("%s play: queue = %u files of '%s', shuffle=%d repeat=%d\n", kTag, static_cast<unsigned>(s_list.count),
                          s_list.cwd, s_shuffle.load() ? 1 : 0, s_repeat.load() ? 1 : 0);
            finish(c.reqId, true, nullptr);
            break;
        }
        case CmdType::QNext: {
            if (s_phase == Phase::Inactive || !s_haveList) {
                finish(c.reqId, false, "queue_idle");
                break;
            }
            uint16_t t = 0;
            s_skipBack = false;
            if (!pickNext(true, &t)) {
                Serial.printf("%s next: end of queue (no repeat) - playback continues\n", kTag);
                finish(c.reqId, false, "end_of_queue");
                break;
            }
            s_skips = 0;
            s_tFlag = 0;
            const char* w = nullptr;
            if (!startTrack(t, &w)) {
                s_cur = t;
                failAndSkip(w);
            }
            finish(c.reqId, true, nullptr);
            break;
        }
        case CmdType::QPrev: {
            if (s_phase == Phase::Inactive || !s_haveList) {
                finish(c.reqId, false, "queue_idle");
                break;
            }
            uint16_t t = 0;
            s_skipBack = true;
            if (!pickPrev(&t)) {
                finish(c.reqId, false, "queue_idle");
                break;
            }
            s_skips = 0;
            s_tFlag = 0;
            const char* w = nullptr;
            if (!startTrack(t, &w)) {
                s_cur = t;
                failAndSkip(w);
            }
            finish(c.reqId, true, nullptr);
            break;
        }
        case CmdType::QStop: {
            if (s_phase == Phase::Inactive) {
                finish(c.reqId, true, nullptr);  // нешкідливо
                break;
            }
            stopQueue("user_stop", true);
            finish(c.reqId, true, nullptr);
            break;
        }
        default:
            break;
    }
}

void tick() {
    using namespace usb_queue;
    const bool shuf = s_shuffle.load();
    if (shuf != s_appliedShuffle) {
        s_appliedShuffle = shuf;
        rebuildForShuffle();
    }
    resumeTick(millis());  // [USB14]
    if (s_startReq.exchange(false)) {
        startPrepared();
    }
    if (s_phase == Phase::Inactive) {
        return;
    }
    const uint32_t now = millis();
    const PlayerState ps = AudioPlayer::state();
    const bool active = AudioPlayer::fileActive();

    if (AudioPlayer::takeFileEnded()) {  // eof (в Starting — дуже короткий файл)
        const uint32_t flag = AudioPlayer::fileEndedAtMs();
        Serial.printf("%s eof seen %u ms after the flag (pos=%d/%u idx=%d)\n", kTag,
                      static_cast<unsigned>(flag != 0 ? now - flag : 0), static_cast<int>(s_pos + 1),
                      static_cast<unsigned>(s_orderLen), static_cast<int>(s_cur));
        s_tFlag = flag;
        // Бібліотека «дограє» і сміттєвий файл без помилки: eof при позиції 0 і тривалості 0 = аудіо не було (файл < 1 с теж).
        if (AudioPlayer::fileDurationSec() == 0 && AudioPlayer::filePositionSec() == 0) {
            failAndSkip("no_audio");
            return;
        }
        onEnded();
        return;
    }

    if (s_phase == Phase::Starting) {
        const uint32_t el = now - s_tAccept;
        if (active && s_tActive == 0) {
            s_tActive = now;
        }
        if (ps != PlayerState::Playing) {
            s_sawLeave = true;
        }
        if (active && ps == PlayerState::Playing && (s_sawLeave || el >= usb_queue_cfg::kStartGraceMs)) {
            s_phase = Phase::Running;  // s_skips скидається лише після треку з аудіо (onEnded), не тут
            ++s_started;
            char a[24], b[24];
            if (s_tFlag != 0) {
                snprintf(a, sizeof(a), "%u", static_cast<unsigned>(s_tAccept - s_tFlag));
                snprintf(b, sizeof(b), "%u", static_cast<unsigned>(now - s_tFlag));
            } else {
                strcpy(a, "n/a");
                strcpy(b, "n/a");
            }
            Serial.printf("%s track started: transition flag->accept=%s accept->active=%u accept->playing=%u flag->playing=%s ms\n",
                          kTag, a, static_cast<unsigned>(s_tActive != 0 ? s_tActive - s_tAccept : 0), static_cast<unsigned>(el), b);
            publish();
            return;
        }
        if (el >= usb_queue_cfg::kStartGraceMs && !active && ps == PlayerState::Error) {
            failAndSkip("open_failed");
            return;
        }
        if (el >= usb_queue_cfg::kStartGuardMs) {
            failAndSkip("start_guard");
            return;
        }
        return;
    }

    // Running
    if (ps == PlayerState::Error && !active) {
        if (!mounted()) {
            stopQueue("media_removed", true);
        } else {
            failAndSkip("read_error");
        }
        return;
    }
    if (ps == PlayerState::Idle && !active) {
        if (s_idleSince == 0) {
            s_idleSince = now;
        } else if (now - s_idleSince >= usb_queue_cfg::kExternalStopDebounceMs) {
            stopQueue("stopped_external", false);
        }
        return;
    }
    s_idleSince = 0;
    {  // [USB14] Пауза НЕ є збоєм: файл активний, стан плеєра Playing. Фронт паузи — знімок позиції й передача в Settings.
        const bool pausedNow = AudioPlayer::filePaused();
        if (pausedNow != s_prevPaused) {
            s_prevPaused = pausedNow;
            refreshSnapshotPos();
            Serial.printf("%s %s at %lu s (idx=%d)\n", kTag, pausedNow ? "PAUSED" : "RESUMED",
                          static_cast<unsigned long>(s_snapPos.load()), static_cast<int>(s_cur));
            if (pausedNow) {
                commitSnapshot("pause");
            }
            publish();
        } else if (!pausedNow && static_cast<uint32_t>(now - s_snapLastMs) >= player_mode_cfg::kSnapshotPeriodMs) {
            refreshSnapshotPos();  // лише RAM, у NVS НЕ пишемо
        }
    }
    if (now - s_lastPathCheck >= usb_queue_cfg::kPathCheckMs) {
        s_lastPathCheck = now;
        char cur[usb_browser_cfg::kPathMax];
        char mine[usb_browser_cfg::kPathMax];
        if (AudioPlayer::currentFilePath(cur, sizeof(cur)) && buildPath(static_cast<uint16_t>(s_cur), mine, sizeof(mine)) &&
            strcmp(cur, mine) != 0) {
            Serial.printf("%s another file is playing ('%s') - queue steps aside\n", kTag, cur);
            stopQueue("superseded", false);
        }
    }
}

}  // namespace q
}  // namespace usb_nav

// ---------------------------------------------------------------------------------------------------------------------
// Публічний API
// ---------------------------------------------------------------------------------------------------------------------
namespace usb_queue {

const char* stateName(State s) {
    switch (s) {
        case State::Idle:     return "Idle";
        case State::Starting: return "Starting";
        case State::Playing:  return "Playing";
    }
    return "?";
}

bool status(Status* out) {
    if (out == nullptr || !usb_nav::lock(usb_browser_cfg::kApiLockWaitMs)) {
        return false;
    }
    *out = s_pub;
    usb_nav::unlock();
    out->shuffle = s_shuffle.load();
    out->repeatAll = s_repeat.load();
    out->paused = AudioPlayer::filePaused();  // [USB14]
    out->prepared = s_prepared.load();
    return true;
}

bool currentPath(char* out, size_t cap) {
    if (out == nullptr || cap == 0 || !usb_nav::lock(usb_browser_cfg::kApiLockWaitMs)) {
        return false;
    }
    bool ok = false;
    if (s_pubPath[0] != '\0' && strlen(s_pubPath) < cap) {
        strcpy(out, s_pubPath);
        ok = true;
    }
    usb_nav::unlock();
    return ok;
}

bool requestPlayIndex(uint16_t browserIndex, uint32_t expectVersion, uint32_t* reqId, const char** why) {
    return usb_nav::post(usb_nav::CmdType::QPlay, browserIndex, expectVersion, reqId, why);
}
bool requestNext(uint32_t* reqId, const char** why) { return usb_nav::post(usb_nav::CmdType::QNext, 0, 0, reqId, why); }
bool requestPrev(uint32_t* reqId, const char** why) { return usb_nav::post(usb_nav::CmdType::QPrev, 0, 0, reqId, why); }
bool requestStop(uint32_t* reqId, const char** why) { return usb_nav::post(usb_nav::CmdType::QStop, 0, 0, reqId, why); }

bool paused() { return AudioPlayer::filePaused(); }

PlayPause requestPlayPause(const char** why) {
    if (why != nullptr) {
        *why = nullptr;
    }
    auto refuse = [&](const char* w) {
        if (why != nullptr) {
            *why = w;
        }
        return PlayPause::Refused;
    };
    if (!usb_nav::modeOk()) {
        return refuse("mode_not_ready");
    }
    Status st;
    if (!status(&st)) {
        return refuse("busy");
    }
    if (st.state == State::Playing) {
        if (!AudioPlayer::fileActive()) {
            return refuse("not_active");
        }
        const bool wantPause = !AudioPlayer::filePaused();
        if (!AudioPlayer::setFilePaused(wantPause)) {
            return refuse("player_refused");
        }
        return wantPause ? PlayPause::Paused : PlayPause::Resumed;
    }
    if (st.state == State::Starting) {
        return refuse("starting");
    }
    if (!s_prepared.load()) {
        return refuse("nothing_to_play");
    }
    s_startReq.store(true);
    return PlayPause::Started;
}

bool requestResume(const char** why) {
    if (why != nullptr) {
        *why = nullptr;
    }
    if (!usb_nav::modeOk()) {
        if (why != nullptr) {
            *why = "mode_not_ready";
        }
        return false;
    }
    s_rsRequested.store(true);
    // QStop на Idle-черзі нічого не робить, але гарантує, що задача usb_nav існує (post() створює її за потреби).
    if (!usb_nav::post(usb_nav::CmdType::QStop, 0, 0, nullptr, why)) {
        s_rsRequested.store(false);
        return false;
    }
    return true;
}

bool commitResume() {
    if (!usb_nav::lock(usb_browser_cfg::kApiLockWaitMs)) {
        return false;
    }
    bool ok = false;
    if (s_sc != nullptr && s_snapValid) {
        SettingsStore::setPlayerResume(PlayerSource::Usb, s_sc->snapDir, s_sc->snapTrack, s_snapPos.load());
        ok = true;
    }
    usb_nav::unlock();
    return ok;
}

void setShuffle(bool on) { s_shuffle.store(on); }
bool shuffle() { return s_shuffle.load(); }
void setRepeatAll(bool on) { s_repeat.store(on); }
bool repeatAll() { return s_repeat.load(); }

#if USB_HOST_DIAG
namespace {

bool word(const char* s, const char* w, const char** rest) {
    const size_t n = strlen(w);
    if (strncmp(s, w, n) != 0 || (s[n] != '\0' && s[n] != ' ' && s[n] != '\t')) {
        return false;
    }
    const char* r = s + n;
    while (*r == ' ' || *r == '\t') {
        ++r;
    }
    *rest = r;
    return true;
}

void waitAndReport(const char* what, bool accepted, uint32_t reqId, const char* why) {
    if (!accepted) {
        Serial.printf("%s %s: REFUSED (%s)\n", kTag, what, why != nullptr ? why : "?");
        return;
    }
    const uint32_t t0 = millis();
    usb_browser::Status st;
    for (;;) {
        if (usb_browser::status(&st) && st.lastReqId >= reqId) {
            break;
        }
        if (static_cast<uint32_t>(millis() - t0) > usb_queue_cfg::kConsoleWaitMs) {
            Serial.printf("%s %s: no result within %lu ms (still pending)\n", kTag, what,
                          static_cast<unsigned long>(usb_queue_cfg::kConsoleWaitMs));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (st.lastReqId == reqId && !st.lastOk) {
        Serial.printf("%s %s: FAILED (%s)\n", kTag, what, st.lastWhy != nullptr ? st.lastWhy : "?");
    } else {
        Serial.printf("%s %s: accepted by the queue in %u ms (watch [USBQ] lines)\n", kTag, what,
                      static_cast<unsigned>(millis() - t0));
    }
}

}  // namespace

// [USB14] Стан паузи, підготовленої черги, відновлення й збережених значень (для usb.q stat і usb.mode stat).
void printPlayerStat() {
    static char dir[player_mode_cfg::kResumePathMax];  // лише консоль (loopTask); не на стеку
    static char trk[player_mode_cfg::kResumePathMax];
    PlayerResume info = {};
    const bool have = SettingsStore::getPlayerResume(PlayerSource::Usb, dir, sizeof(dir), trk, sizeof(trk), &info);
    const Settings s = SettingsStore::snapshot();
    Serial.printf("%s player: paused=%d prepared=%d resume_state=%d outcome=%s | saved shuffle=%d repeat=%d | saved pos: %s%s%s / %s%s%s @ %lu s\n",
                  kTag, AudioPlayer::filePaused() ? 1 : 0, s_prepared.load() ? 1 : 0, static_cast<int>(s_rs),
                  s_rsOutcome != nullptr ? s_rsOutcome : "-", s.playerShuffle ? 1 : 0, s.playerRepeat ? 1 : 0, have ? "'" : "-",
                  have ? dir : "", have ? "'" : "", have ? "'" : "-", have ? trk : "", have ? "'" : "",
                  static_cast<unsigned long>(have ? info.posSec : 0));
    Serial.printf("%s player: NVS writes this session: resume keys=%lu blob=%lu | queue commits=%lu | dirty=%d\n", kTag,
                  static_cast<unsigned long>(SettingsStore::playerResumeWriteCount()),
                  static_cast<unsigned long>(SettingsStore::writeCount()), static_cast<unsigned long>(s_resumeCommits),
                  SettingsStore::isDirty() ? 1 : 0);
}

namespace {

void printStat() {
    Status q;
    usb_browser::Status b;
    const bool okq = status(&q);
    const bool okb = usb_browser::status(&b);
    if (!okq || !okb) {
        Serial.printf("%s stat: busy, repeat\n", kTag);
        return;
    }
    Serial.printf("%s stat: mode=%s queue=%s count=%u idx=%ld pos=%ld shuffle=%d repeat=%d skips=%u started=%lu reason=%s v=%lu\n",
                  kTag, usb_mode::stateName(usb_mode::state()), stateName(q.state), static_cast<unsigned>(q.count),
                  static_cast<long>(q.index), static_cast<long>(q.position), q.shuffle ? 1 : 0, q.repeatAll ? 1 : 0,
                  static_cast<unsigned>(q.skips), static_cast<unsigned long>(q.started), q.reason != nullptr ? q.reason : "-",
                  static_cast<unsigned long>(q.version));
    char path[usb_browser_cfg::kPathMax];
    const bool havePath = currentPath(path, sizeof(path));
    Serial.printf("%s stat: now=%s%s%s | player=%s file_active=%d pos=%lu/%lu s\n", kTag, havePath ? "'" : "-", havePath ? path : "",
                  havePath ? "'" : "", AudioPlayer::stateName(AudioPlayer::state()), AudioPlayer::fileActive() ? 1 : 0,
                  static_cast<unsigned long>(AudioPlayer::filePositionSec()), static_cast<unsigned long>(AudioPlayer::fileDurationSec()));
    Serial.printf("%s stat: browser active=%d valid=%d count=%u dirs=%u depth=%u trunc=%d v=%lu read=%lu ms pending=%u last_req=%lu ok=%d why=%s err=%s\n",
                  kTag, b.active ? 1 : 0, b.valid ? 1 : 0, static_cast<unsigned>(b.count), static_cast<unsigned>(b.dirs),
                  static_cast<unsigned>(b.depth), b.truncated ? 1 : 0, static_cast<unsigned long>(b.version),
                  static_cast<unsigned long>(b.readMs), static_cast<unsigned>(b.pending), static_cast<unsigned long>(b.lastReqId),
                  b.lastOk ? 1 : 0, b.lastWhy != nullptr ? b.lastWhy : "-", b.lastError != nullptr ? b.lastError : "-");
    const TaskHandle_t h = xTaskGetHandle(usb_browser_cfg::kTaskName);
    if (h != nullptr) {
        Serial.printf("%s stat: task '%s' stack min free=%u B of %u\n", kTag, usb_browser_cfg::kTaskName,
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(h)), static_cast<unsigned>(usb_browser_cfg::kTaskStack));
    } else {
        Serial.printf("%s stat: task '%s' not running\n", kTag, usb_browser_cfg::kTaskName);
    }
    printPlayerStat();  // [USB14]
    Serial.printf("%s stat: heap internal free=%u largest=%u min_free=%u psram_free=%u\n", kTag,
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

}  // namespace

bool isConsoleCommand(const char* line) {
    const char* rest = nullptr;
    return line != nullptr && word(line, "usb.q", &rest);
}

void handleConsoleLine(const char* line) {
    const char* rest = nullptr;
    if (!word(line, "usb.q", &rest)) {
        Serial.printf("%s unknown command\n", kTag);
        return;
    }
    const char* arg = nullptr;
    uint32_t id = 0;
    const char* why = nullptr;
    if (word(rest, "play", &arg)) {
        if (arg[0] < '0' || arg[0] > '9') {
            Serial.printf("%s usage: usb.q play <index of a FILE in 'usb.dir'>\n", kTag);
            return;
        }
        const bool ok = requestPlayIndex(static_cast<uint16_t>(atol(arg)), 0, &id, &why);
        waitAndReport("play", ok, id, why);
    } else if (word(rest, "next", &arg)) {
        const bool ok = requestNext(&id, &why);
        waitAndReport("next", ok, id, why);
    } else if (word(rest, "prev", &arg)) {
        const bool ok = requestPrev(&id, &why);
        waitAndReport("prev", ok, id, why);
    } else if (word(rest, "stop", &arg)) {
        const bool ok = requestStop(&id, &why);
        waitAndReport("stop", ok, id, why);
    } else if (word(rest, "stat", &arg)) {
        printStat();
    } else if (word(rest, "shuffle", &arg)) {
        if (strcmp(arg, "on") == 0) {
            setShuffle(true);
        } else if (strcmp(arg, "off") == 0) {
            setShuffle(false);
        } else if (arg[0] != '\0') {
            Serial.printf("%s usage: usb.q shuffle on|off\n", kTag);
            return;
        }
        Serial.printf("%s shuffle=%d (applied by the worker within ~%u ms)\n", kTag, shuffle() ? 1 : 0,
                      static_cast<unsigned>(usb_browser_cfg::kPollMs));
    } else if (word(rest, "repeat", &arg)) {
        if (strcmp(arg, "on") == 0) {
            setRepeatAll(true);
        } else if (strcmp(arg, "off") == 0) {
            setRepeatAll(false);
        } else if (arg[0] != '\0') {
            Serial.printf("%s usage: usb.q repeat on|off\n", kTag);
            return;
        }
        Serial.printf("%s repeat=%d\n", kTag, repeatAll() ? 1 : 0);
    } else {
        Serial.printf("%s usage: usb.q play <index> | next | prev | stop | stat | shuffle on|off | repeat on|off\n", kTag);
    }
}
#endif  // USB_HOST_DIAG

}  // namespace usb_queue

#endif  // FEATURE_USB_MODE
