// [USB10] Черга відтворення: автоперехід, послідовний режим / перемішування, повтор, пропуск пошкоджених файлів.
// Див. usb_queue.h (правила), config/usb_queue_config.h. Уся логіка черги виконується в задачі usb_nav (usb_browser.cpp);
// публічні функції лише ставлять команди й читають опублікований знімок стану.
//
// Автоперехід: кожні kPollMs задача перевіряє AudioPlayer::takeFileEnded() (прапорець піднімається ПІСЛЯ kEndTailMs, мʼюту ЦАП
// і Idle — USB7), далі вибирає наступний трек і викликає AudioPlayer::playFile(). Час переходу друкується в логу
// («[USBQ] track ... transition: flag->accept=… accept->active=… accept->playing=… flag->playing=… ms»).

// [USB20a-1] Опція scope (folder / all) і поля Status::builtScope / truncated / building.
// [USB20a-2] Черга «all»: у режимі scope = all play файлу X будує СПОЧАТКУ звичайну папкову чергу (X стартує одразу), а скан усієї
// флешки (обхід у глибину: файли папки, потім підпапки; порядок сортування як у браузері) іде кроками в tick(); коли X знайдено в
// загальному порядку, черга одним кроком підміняється на «all» (звук не переривається). Поки скан іде: next / prev -> «queue_building»,
// shuffle застосовується після скану, автоперехід по кінцю треку чекає. Не знайдено X (ліміт / глибина / помилка) — папкова черга.

#include "usb/usb_queue.h"

#if FEATURE_USB_MODE

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "audio/audio_player.h"
#include "config/player_mode_config.h"  // [USB14]
#include "config/usb_queue_config.h"
#include "config/usb_queue_all_config.h"    // [USB20a-2]
#include "config/usb_library_config.h"      // [USB22a-1]
#include "config/usb_queue_scope_config.h"  // [USB20a-1]
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
static_assert(usb_queue_all_cfg::kQueueAllMax >= usb_browser_cfg::kMaxEntries,
              "[USB20a-2] s_order is sized for the full queue; it must also hold a folder queue");

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
std::atomic<uint8_t> s_scope{static_cast<uint8_t>(usb_queue_scope_cfg::kDefaultScope)};  // [USB20a-1] опція (не область побудованої черги)
bool s_truncated = false;  // [USB20a-1] список, з якого побудовано чергу, був обрізаний (лише usb_nav)

// --- [USB20a-2] Черга «усе»: таблиці файлів і папок усієї флешки ------------------------------------------------------------
// Усе в PSRAM; виділяється ЛІНИВО при першому play у режимі «all» (allocAll), вивільняється в onWorkerStop (freeAll). Шлях файлу не
// зберігається цілком: файл знає свою папку (dirIdx), а папки утворюють дерево (parent) з імʼями в спільному пулі.
struct AllFile {
    uint32_t nameOff;  // імʼя в пулі (з '\0')
    uint16_t dirIdx;   // індекс папки в dirs
    uint16_t pad;
};  // 8 Б
struct AllDir {
    uint32_t nameOff;
    uint16_t parent;      // kNoNode — корінь
    uint16_t firstChild;  // підпапки однієї папки лежать ПІДРЯД: [firstChild, firstChild + childCount)
    uint16_t childCount;
    uint8_t depth;        // 0 — корінь
    uint8_t pad;
};  // 12 Б
struct AllCtx {
    AllFile* files;
    AllDir* dirs;
    char* pool;
    uint16_t nFiles;
    uint16_t nDirs;
    uint32_t poolUsed;
    usb_nav::Listing scan;                    // буфер readDir (e / pool у PSRAM)
    char scanPath[usb_browser_cfg::kPathMax];  // шлях папки, що читається
    char xDir[usb_browser_cfg::kPathMax];      // папка й імʼя файлу X, з якого стартувала черга «all»
    char xName[usb_browser_cfg::kPathMax];
};
constexpr uint16_t kNoNode = 0xFFFF;

AllCtx* s_all = nullptr;      // лінивий (лише usb_nav)
bool s_builtAll = false;      // черга побудована як «all»: індекси треків — у s_all->files
bool s_building = false;      // скан іде; черга тим часом папкова
bool s_endWait = false;       // трек закінчився, поки скан іде: автоперехід чекає кінця скану
bool s_allTruncated = false;  // скан відкинув частину вмісту (ліміти / глибина / помилка читання / час)
bool s_capHit = false;        // досягнуто ліміт файлів або пулу: скан зупиняється
int32_t s_xOrdinal = -1;      // номер файлу X у таблиці «all» (-1 — ще не знайдено)
uint16_t s_scanNode = 0;      // папка, яку читатимемо наступною
uint32_t s_scanGen = 0;       // покоління монтування на початку скану
uint32_t s_scanT0 = 0;
uint32_t s_scanSteps = 0;
uint32_t s_scanBusy = 0;
uint16_t s_scanSkipped = 0;   // папки, які не вдалося прочитати
uint8_t s_busyRetries = 0;
bool s_scanHold = false;      // скан чекає, поки X заграє (або мине kScanStartGraceMs)
uint16_t s_scanDelayMs = 0;   // ДІАГНОСТИКА: пауза після кожної прочитаної папки (usb.q scandelay <мс>); 0 — вимкнено
uint32_t s_scanReqMs = 0;     // момент play, що почав скан

void cancelBuild(const char* why);
void abortScan(const char* why);  // [USB22a-1]

// --- [USB22a-1] Бібліотека: ті самі таблиці s_all, але скан не прив'язаний до черги ------------------------------------------
// s_building = «черга чекає на скан» (як у USB20a-2); s_scanActive = «скан таблиць іде» (для черги, бібліотеки або обох);
// s_libWant = бібліотека зацікавлена в скані (скан не скасовується, коли черга від нього відмовилась). Усе — лише usb_nav.
// s_lib — знімок для читача (async_tcp): пишеться ЛИШЕ під usb_nav::lock. Читач бачить таблиці тільки коли state == Ready;
// з Ready їх можна змінити лише після libSet/libReset(None) під тим самим lock, тож читач не бачить недописаних даних.
bool s_scanActive = false;
bool s_libWant = false;
std::atomic<uint32_t> s_libReqAt{0};  // мітка часу запиту бібліотеки (0 — немає); виставляє async_tcp, споживає tick()

struct LibPub {
    LibState state;
    uint32_t version;
    uint16_t nFiles;
    uint16_t nDirs;
    bool truncated;
    const char* reason;
};
LibPub s_lib = {LibState::None, 0, 0, 0, false, nullptr};

void libSet(LibState st, uint16_t nf, uint16_t nd, bool trunc, const char* reason, bool bump) {
    usb_nav::lock(UINT32_MAX);
    s_lib.state = st;
    s_lib.nFiles = nf;
    s_lib.nDirs = nd;
    s_lib.truncated = trunc;
    s_lib.reason = reason;
    if (bump) {
        ++s_lib.version;
    }
    usb_nav::unlock();
}

// Прогрес скану для відповіді «building» (лише лічильники; таблиці читач не бачить).
void libProgress() {
    usb_nav::lock(UINT32_MAX);
    s_lib.nFiles = s_all->nFiles;
    s_lib.nDirs = s_all->nDirs;
    usb_nav::unlock();
}

// Кеш недійсний (зміна носія / скасований скан / повторний скан). version росте, лише якщо щось було.
void libReset() {
    usb_nav::lock(UINT32_MAX);
    if (s_lib.state != LibState::None) {
        ++s_lib.version;
    }
    s_lib.state = LibState::None;
    s_lib.nFiles = 0;
    s_lib.nDirs = 0;
    s_lib.truncated = false;
    s_lib.reason = nullptr;
    usb_nav::unlock();
}

// Скан більше нікому не потрібен: зупиняємо й відкидаємо недобудовані таблиці.
void stopScan() {
    s_scanActive = false;
    s_scanHold = false;
    libReset();
}

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
    char tmpDir[player_mode_cfg::kResumePathMax];     // [USB20a-2] папка треку черги «all» (лише usb_nav; перед копіюванням у snapDir)
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

// [USB20a-2] Єдиний доступ до черги: папкова (s_list) або «all» (s_all). Індекс t завжди < qCount().
inline uint16_t qCount() { return s_builtAll ? (s_all != nullptr ? s_all->nFiles : 0) : s_list.count; }
inline const char* qName(uint16_t t) {
    return s_builtAll ? s_all->pool + s_all->files[t].nameOff : usb_nav::nameAt(s_list, t);
}

// Шлях папки таблиці «all»: "/" або "/a/b" (без '/' наприкінці; той самий формат, що Listing::cwd).
bool allDirPath(uint16_t idx, char* out, size_t cap) {
    const AllCtx* a = s_all;
    if (a == nullptr || idx >= a->nDirs || cap < 2) {
        return false;
    }
    uint16_t chain[usb_browser_cfg::kMaxDepth + 1];
    size_t n = 0;
    for (uint16_t cur = idx; cur != 0; cur = a->dirs[cur].parent) {
        if (n >= sizeof(chain) / sizeof(chain[0])) {
            return false;
        }
        chain[n++] = cur;
    }
    if (n == 0) {
        out[0] = '/';
        out[1] = '\0';
        return true;
    }
    size_t off = 0;
    for (size_t i = n; i > 0; --i) {
        const int w = snprintf(out + off, cap - off, "/%s", a->pool + a->dirs[chain[i - 1]].nameOff);
        if (w <= 0 || static_cast<size_t>(w) >= cap - off) {
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

bool buildPath(uint16_t t, char* out, size_t cap) {
    if (t >= qCount()) {
        return false;
    }
    if (!s_builtAll) {
        const char* dir = s_list.cwd;
        const bool root = (dir[0] == '/' && dir[1] == '\0');
        const int n = snprintf(out, cap, "%s%s%s", dir, root ? "" : "/", usb_nav::nameAt(s_list, t));
        return n > 0 && static_cast<size_t>(n) < cap;
    }
    const AllFile& f = s_all->files[t];
    if (!allDirPath(f.dirIdx, out, cap)) {
        return false;
    }
    const size_t len = strlen(out);
    const bool root = (len == 1);
    const int n = snprintf(out + len, cap - len, "%s%s", root ? "" : "/", s_all->pool + f.nameOff);
    return n > 0 && static_cast<size_t>(n) < cap - len;
}

void publish() {
    Status st = {};
    st.state = (s_phase == Phase::Inactive) ? State::Idle : (s_phase == Phase::Starting ? State::Starting : State::Playing);
    st.count = s_haveList ? qCount() : 0;  // [USB20a-2]
    st.index = (s_phase == Phase::Inactive) ? -1 : s_cur;
    st.position = (s_phase == Phase::Inactive) ? -1 : s_pos;
    // [USB18a] pos: де поточний трек у ПОТОЧНОМУ порядку проходу. s_pos не оновлюється на шляхах історії (prev / next-назад у
    // shuffle), тож місце шукається в s_order (лише при зміні стану; <= kMaxEntries кроків).
    st.pos = 0;
    if (s_phase != Phase::Inactive && s_cur >= 0 && s_order != nullptr) {
        for (uint16_t i = 0; i < s_orderLen; ++i) {
            if (s_order[i] == static_cast<uint16_t>(s_cur)) {
                st.pos = static_cast<int32_t>(i) + 1;
                break;
            }
        }
    }
    st.skips = s_skips;
    st.started = s_started;
    st.reason = s_reason;
    st.paused = false;
    st.prepared = false;
    st.builtScope = s_haveList ? (s_builtAll ? 2 : 1) : 0;  // [USB20a-2] 0 — нема, 1 — folder, 2 — all
    st.truncated = s_haveList && s_truncated;
    st.building = s_building;  // [USB20a-2]
    s_prepared.store(s_phase == Phase::Inactive && s_haveList && qCount() > 0 && s_cur >= 0);
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
    const uint16_t n = qCount();  // [USB20a-2]
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
    if (s_sc == nullptr || t >= qCount()) {
        return;
    }
    const char* name = qName(t);
    const char* dir = s_list.cwd;
    if (s_builtAll) {  // [USB20a-2] папка треку черги «all» — з дерева папок
        if (!allDirPath(s_all->files[t].dirIdx, s_sc->tmpDir, sizeof(s_sc->tmpDir))) {
            return;
        }
        dir = s_sc->tmpDir;
    }
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
    cancelBuild("queue_stopped");  // [USB20a-2] скан «all» не продовжується без черги
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
    const uint16_t n = qCount();  // [USB20a-2]
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
    const uint16_t n = qCount();  // [USB20a-2]
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
    cancelBuild("track_failed");  // [USB20a-2] поточний трек змінюється: скан «all» (орієнтований на X) більше не має сенсу
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

// --- [USB20a-2] Черга «усе»: рекурсивний скан у фоні ----------------------------------------------------------------------------
// Скан іде кроками в tick(): за такт — не довше kScanSliceMs (плюс дочитування поточної папки), КОЖЕН крок = читання ОДНІЄЇ папки
// наявним readDir (ті самі фільтри / службові імена / сортування, що в браузері). Між тактами usb_nav обробляє нові команди й перевіряє
// носій, тож скан переривається вийманням носія, виходом із режиму, новим play, stop. Обхід у глибину: файли папки, потім підпапки.
bool allocAll() {
    if (s_all != nullptr) {
        return true;
    }
    AllCtx* a = static_cast<AllCtx*>(heap_caps_malloc(sizeof(AllCtx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (a == nullptr) {
        return false;
    }
    memset(a, 0, sizeof(AllCtx));
    a->files = static_cast<AllFile*>(
        heap_caps_malloc(sizeof(AllFile) * usb_queue_all_cfg::kQueueAllMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    a->dirs = static_cast<AllDir*>(
        heap_caps_malloc(sizeof(AllDir) * usb_queue_all_cfg::kQueueAllDirMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    a->pool = static_cast<char*>(heap_caps_malloc(usb_queue_all_cfg::kQueueAllPoolBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (a->files == nullptr || a->dirs == nullptr || a->pool == nullptr || !usb_nav::allocListing(a->scan)) {
        usb_nav::freeListing(a->scan);
        heap_caps_free(a->files);
        heap_caps_free(a->dirs);
        heap_caps_free(a->pool);
        heap_caps_free(a);
        return false;
    }
    s_all = a;
    return true;
}

void freeAll() {
    // [USB22a-1] s_all і знімок бібліотеки скидаються ПІД lock: читач (async_tcp) або бачить усе, або нічого.
    usb_nav::lock(UINT32_MAX);
    AllCtx* a = s_all;
    s_all = nullptr;
    s_builtAll = false;
    s_scanActive = false;
    s_libWant = false;
    if (s_lib.state != LibState::None) {
        ++s_lib.version;
    }
    s_lib.state = LibState::None;
    s_lib.nFiles = 0;
    s_lib.nDirs = 0;
    s_lib.truncated = false;
    s_lib.reason = nullptr;
    usb_nav::unlock();
    if (a == nullptr) {
        return;
    }
    usb_nav::freeListing(a->scan);
    heap_caps_free(a->files);
    heap_caps_free(a->dirs);
    heap_caps_free(a->pool);
    heap_caps_free(a);
}

void resetAll() {
    AllCtx* a = s_all;
    a->nFiles = 0;
    a->nDirs = 1;  // корінь
    a->poolUsed = 1;
    a->pool[0] = '\0';  // імʼя кореня — порожнє
    a->dirs[0] = AllDir{0, kNoNode, 0, 0, 0, 0};
}

// Наступна папка за обходом у глибину (підпапки лежать підряд): перша підпапка, інакше наступна «сестра» найближчого предка.
uint16_t nextNode(uint16_t node) {
    const AllCtx* a = s_all;
    if (a->dirs[node].childCount > 0) {
        return a->dirs[node].firstChild;
    }
    uint16_t n = node;
    while (n != 0) {
        const uint16_t p = a->dirs[n].parent;
        if (static_cast<uint32_t>(n) + 1 < static_cast<uint32_t>(a->dirs[p].firstChild) + a->dirs[p].childCount) {
            return static_cast<uint16_t>(n + 1);
        }
        n = p;
    }
    return kNoNode;
}

// Переносить щойно прочитану папку (a->scan) у таблиці: спершу її файли (у порядку списку браузера), потім створює записи підпапок.
void absorb(uint16_t node, bool isXDir) {
    AllCtx* a = s_all;
    const usb_nav::Listing& L = a->scan;
    for (uint16_t i = L.dirs; i < L.count; ++i) {
        const char* nm = usb_nav::nameAt(L, i);
        const uint32_t n = L.e[i].nameLen;
        if (a->nFiles >= usb_queue_all_cfg::kQueueAllMax || a->poolUsed + n + 1 > usb_queue_all_cfg::kQueueAllPoolBytes) {
            s_allTruncated = true;
            s_capHit = true;
            return;
        }
        if (isXDir && s_xOrdinal < 0 && strcmp(nm, a->xName) == 0) {
            s_xOrdinal = static_cast<int32_t>(a->nFiles);
        }
        AllFile& f = a->files[a->nFiles++];
        f.nameOff = a->poolUsed;
        f.dirIdx = node;
        f.pad = 0;
        memcpy(a->pool + a->poolUsed, nm, n + 1);
        a->poolUsed += n + 1;
    }
    if (L.dirs > 0) {
        if (a->dirs[node].depth >= usb_browser_cfg::kMaxDepth) {
            s_allTruncated = true;  // глибше за браузер (too_deep) не заходимо
        } else {
            const uint16_t first = a->nDirs;
            for (uint16_t i = 0; i < L.dirs; ++i) {
                const char* nm = usb_nav::nameAt(L, i);
                const uint32_t n = L.e[i].nameLen;
                if (a->nDirs >= usb_queue_all_cfg::kQueueAllDirMax || a->poolUsed + n + 1 > usb_queue_all_cfg::kQueueAllPoolBytes) {
                    s_allTruncated = true;
                    break;
                }
                AllDir& d = a->dirs[a->nDirs++];
                d.nameOff = a->poolUsed;
                d.parent = node;
                d.firstChild = 0;
                d.childCount = 0;
                d.depth = static_cast<uint8_t>(a->dirs[node].depth + 1);
                d.pad = 0;
                memcpy(a->pool + a->poolUsed, nm, n + 1);
                a->poolUsed += n + 1;
            }
            a->dirs[node].firstChild = first;
            a->dirs[node].childCount = static_cast<uint16_t>(a->nDirs - first);
        }
    }
    if (L.truncated) {
        s_allTruncated = true;  // папка має > kMaxEntries записів / не вмістилась у пул браузера
    }
}

enum class Step : uint8_t { More, Retry, Done };

Step scanStepOne() {
    AllCtx* a = s_all;
    const uint16_t node = s_scanNode;
    if (!allDirPath(node, a->scanPath, sizeof(a->scanPath))) {
        s_allTruncated = true;
        ++s_scanSkipped;
    } else {
        const usb_nav::ReadResult r = usb_nav::readDir(a->scanPath, a->dirs[node].depth, a->scan);
        ++s_scanSteps;
        if (r == usb_nav::ReadResult::VolumeBusy) {  // том зайнятий декодером довше kGenLockWaitMs: повторимо наступного такту
            ++s_scanBusy;
            if (++s_busyRetries <= usb_queue_all_cfg::kScanBusyRetryMax) {
                return Step::Retry;
            }
            s_allTruncated = true;
            ++s_scanSkipped;
            Serial.printf("%s all: dir '%s' skipped (volume busy)\n", kTag, a->scanPath);
        } else if (r == usb_nav::ReadResult::NotMounted || r == usb_nav::ReadResult::MediaChanged ||
                   r == usb_nav::ReadResult::ModeGone) {
            abortScan(usb_nav::readResultName(r));  // [USB22a-1] скасовує й скан бібліотеки
            publish();
            return Step::Retry;
        } else if (r != usb_nav::ReadResult::Ok) {
            s_allTruncated = true;
            ++s_scanSkipped;
            Serial.printf("%s all: dir '%s' skipped (%s)\n", kTag, a->scanPath, usb_nav::readResultName(r));
        } else {
            absorb(node, strcmp(a->scanPath, a->xDir) == 0);
            libProgress();  // [USB22a-1]
        }
    }
    s_busyRetries = 0;
    if (s_scanDelayMs != 0) {  // ДІАГНОСТИКА: розтягує скан, щоб встигнути перервати його (вийняття носія, вихід із режиму)
        vTaskDelay(pdMS_TO_TICKS(s_scanDelayMs));
    }
    if (s_capHit) {
        return Step::Done;
    }
    s_scanNode = nextNode(node);
    return s_scanNode == kNoNode ? Step::Done : Step::More;
}

void cancelBuild(const char* why) {
    if (!s_building) {
        return;
    }
    s_building = false;
    s_endWait = false;
    Serial.printf("%s all: scan cancelled (%s) after %u ms - the queue stays per folder\n", kTag, why,
                  static_cast<unsigned>(millis() - s_scanT0));
    if (s_libWant) {  // [USB22a-1] скан далі потрібен бібліотеці: він триває як скан без X
        s_scanHold = false;
        Serial.printf("%s all: the scan continues for the library\n", kTag);
        return;
    }
    stopScan();
}

// [USB22a-1] Скасування скану цілком (зміна носія / вихід з режиму / помилка читання): і для черги, і для бібліотеки.
void abortScan(const char* why) {
    const bool lib = s_libWant;
    s_libWant = false;
    cancelBuild(why);  // без s_libWant зупиняє й сам скан
    if (s_scanActive) {
        stopScan();
    }
    if (lib) {
        Serial.printf("%s library: scan aborted (%s)\n", kTag, why);
    }
}

// Скан завершено (вся флешка, ліміт або час). X знайдено — черга підміняється на «all»; ні — лишається папкова.
void finishScan() {
    const AllCtx* a = s_all;
    const uint32_t ms = millis() - s_scanT0;
    const bool queueWants = s_building;  // [USB22a-1] скан міг іти лише для бібліотеки (X не потрібен)
    s_building = false;
    s_scanActive = false;
    s_libWant = false;
    s_scanHold = false;
    const bool found = queueWants && s_xOrdinal >= 0;
    if (found) {
        s_builtAll = true;
        s_cur = s_xOrdinal;
        clearHistory();
        s_appliedShuffle = s_shuffle.load();
        buildOrder(s_xOrdinal, "all");
        s_truncated = s_allTruncated;
    } else if (queueWants) {
        s_truncated = s_truncated || s_allTruncated;
    }
    // [USB22a-1] Кеш бібліотеки готовий незалежно від того, чи знайдено X (таблиці повні в межах лімітів).
    libSet(a->nFiles == 0 ? LibState::Empty : LibState::Ready, a->nFiles, a->nDirs, s_allTruncated, nullptr, true);
    Serial.printf("%s all: scan done in %u ms: %u files in %u dirs (steps=%u busy=%u skipped_dirs=%u truncated=%d pool=%u/%u B); "
                  "X %s (ordinal %d) -> queue scope=%s count=%u; psram_free=%u\n",
                  kTag, static_cast<unsigned>(ms), static_cast<unsigned>(a->nFiles), static_cast<unsigned>(a->nDirs),
                  static_cast<unsigned>(s_scanSteps), static_cast<unsigned>(s_scanBusy), static_cast<unsigned>(s_scanSkipped),
                  s_allTruncated ? 1 : 0, static_cast<unsigned>(a->poolUsed),
                  static_cast<unsigned>(usb_queue_all_cfg::kQueueAllPoolBytes),
                  !queueWants ? "n/a (library scan)" : (found ? "found" : "NOT found"),
                  static_cast<int>(s_xOrdinal), found ? "all" : (queueWants ? "folder" : "unchanged"), static_cast<unsigned>(qCount()),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    publish();
    if (s_endWait) {
        s_endWait = false;
        Serial.printf("%s all: the auto-advance that waited for the scan continues now\n", kTag);
        onEnded();
    }
}

// Один такт скану (з tick()).
void scanTick() {
    if (!s_scanActive) {  // [USB22a-1] скан іде для черги та/або бібліотеки
        return;
    }
    const uint32_t gen = usb_nav::generation();
    if (!usb_nav::modeOk() || !mounted() || (gen != usb_nav::kGenUnknown && gen != s_scanGen)) {
        abortScan("mode_or_media");
        publish();
        return;
    }
    if (s_scanHold) {  // не читаємо флешку, поки X відкривається: інакше його старт повільніший на ≈ 0,5 с
        if (s_phase != Phase::Running && static_cast<uint32_t>(millis() - s_scanReqMs) < usb_queue_all_cfg::kScanStartGraceMs) {
            return;
        }
        s_scanHold = false;
        s_scanT0 = millis();  // «scan done in N ms» міряє сам скан
    }
    const uint32_t t0 = millis();
    do {
        if (static_cast<uint32_t>(t0 - s_scanT0) >= usb_queue_all_cfg::kScanMaxMs) {
            s_allTruncated = true;
            Serial.printf("%s all: scan time cap reached (%u ms)\n", kTag, static_cast<unsigned>(usb_queue_all_cfg::kScanMaxMs));
            finishScan();
            return;
        }
        const Step st = scanStepOne();
        if (!s_scanActive) {  // [USB22a-1]
            return;
        }
        if (st == Step::Done) {
            finishScan();
            return;
        }
        if (st == Step::Retry) {
            return;
        }
    } while (static_cast<uint32_t>(millis() - t0) < usb_queue_all_cfg::kScanSliceMs);
}

// Початок скану з play файлу index (індекс запису в живому списку браузера lv). false — «all» недоступна (черга лишається папковою).
// [USB22a-1] Спільний старт скану (черга й бібліотека): таблиці виділено, кеш скинуто, лічильники обнулено.
// nullptr — скан запущено; інакше причина відмови (статичний рядок). Викликати лише з usb_nav.
const char* startScanCommon() {
    if (!allocAll()) {
        return "no_psram";
    }
    uint32_t gen = usb_nav::generation();
    if (gen == usb_nav::kGenUnknown) {
        gen = usb_nav::generation();
    }
    if (gen == 0 || gen == usb_nav::kGenUnknown) {
        return "volume_not_readable";
    }
    libReset();  // кеш недійсний ДО перезапису таблиць
    resetAll();
    s_all->xDir[0] = '\0';
    s_all->xName[0] = '\0';
    s_xOrdinal = -1;
    s_allTruncated = false;
    s_capHit = false;
    s_scanNode = 0;
    s_busyRetries = 0;
    s_scanSteps = 0;
    s_scanBusy = 0;
    s_scanSkipped = 0;
    s_scanGen = gen;
    s_scanReqMs = millis();
    s_scanT0 = s_scanReqMs;
    s_scanHold = false;
    s_endWait = false;
    s_scanActive = true;
    libSet(LibState::Building, 0, 0, false, nullptr, false);
    return nullptr;
}

bool beginScan(const usb_nav::Listing& lv, uint16_t index) {
    const char* fail = startScanCommon();
    if (fail != nullptr) {
        Serial.printf("%s all: unavailable (%s) - the queue stays per folder\n", kTag,
                      strcmp(fail, "no_psram") == 0 ? "no PSRAM for the full-queue tables" : "volume not readable now");
        return false;
    }
    strncpy(s_all->xDir, lv.cwd, sizeof(s_all->xDir) - 1);
    s_all->xDir[sizeof(s_all->xDir) - 1] = '\0';
    strncpy(s_all->xName, usb_nav::nameAt(lv, index), sizeof(s_all->xName) - 1);
    s_all->xName[sizeof(s_all->xName) - 1] = '\0';
    s_scanHold = true;
    s_building = true;
    Serial.printf("%s all: scan armed (X='%s' in '%s'; starts when the track plays; limits: %u files, %u dirs, depth %u, %u ms)\n", kTag, s_all->xName, s_all->xDir,
                  static_cast<unsigned>(usb_queue_all_cfg::kQueueAllMax), static_cast<unsigned>(usb_queue_all_cfg::kQueueAllDirMax),
                  static_cast<unsigned>(usb_browser_cfg::kMaxDepth), static_cast<unsigned>(usb_queue_all_cfg::kScanMaxMs));
    return true;
}

// [USB22a-1] Споживає запит бібліотеки (з tick()). Скан уже йде — приєднується; кеш є / помилка — нічого; інакше запускає скан без X.
void libTick() {
    const uint32_t t = s_libReqAt.exchange(0);
    if (t == 0) {
        return;
    }
    if (static_cast<uint32_t>(millis() - t) > usb_library_cfg::kReqStaleMs || !usb_nav::modeOk() || !mounted()) {
        return;
    }
    if (s_scanActive) {
        if (!s_libWant) {
            s_libWant = true;
            Serial.printf("%s library: joined the running scan\n", kTag);
        }
        return;
    }
    if (s_lib.state != LibState::None) {  // Ready / Empty / Error: нічого робити (Error — до зміни носія)
        return;
    }
    const char* fail = startScanCommon();
    if (fail != nullptr) {
        libSet(LibState::Error, 0, 0, false, fail, true);
        Serial.printf("%s library: unavailable (%s)\n", kTag, fail);
        return;
    }
    s_libWant = true;
    Serial.printf("%s library: scan started (limits: %u files, %u dirs, depth %u, %u ms)\n", kTag,
                  static_cast<unsigned>(usb_queue_all_cfg::kQueueAllMax), static_cast<unsigned>(usb_queue_all_cfg::kQueueAllDirMax),
                  static_cast<unsigned>(usb_browser_cfg::kMaxDepth), static_cast<unsigned>(usb_queue_all_cfg::kScanMaxMs));
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
    s_truncated = lv.truncated;  // [USB20a-1]
    cancelBuild("resume prepare");  // [USB20a-2] відновлення будує ПАПКОВУ чергу (scope діє на наступний play)
    s_builtAll = false;
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
    if (s_phase != Phase::Inactive || !s_haveList || s_cur < 0 || qCount() == 0) {
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
    if (!s_haveList || qCount() == 0 || s_order == nullptr) {
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
    s_builtAll = false;  // [USB20a-2]
    s_building = false;
    s_endWait = false;
    s_scanActive = false;  // [USB22a-1] (s_libReqAt НЕ чіпаємо: запит міг прийти до створення задачі)
    s_libWant = false;
    clearHistory();
    if (!allocListing(s_list)) {
        return false;
    }
    s_order = static_cast<uint16_t*>(
        heap_caps_malloc(sizeof(uint16_t) * usb_queue_all_cfg::kQueueAllMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));  // [USB20a-2]
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
    abortScan("mode_exit");  // [USB20a-2][USB22a-1] і скан бібліотеки
    s_libReqAt.store(0);
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
    freeAll();  // [USB20a-2]
    freeListing(s_list);
    Serial.printf("%s worker stack min free=%u B\n", kTag, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void onMediaGone() {
    using namespace usb_queue;
    abortScan("media_gone");  // [USB20a-2][USB22a-1] і скан бібліотеки
    libReset();               // [USB22a-1] кеш іншого носія недійсний (також скидає Error)
    s_builtAll = false;
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
            s_truncated = lv.truncated;  // [USB20a-1]
            cancelBuild("new play");  // [USB20a-2] нова черга замінює стару (і скан, що ще йшов)
            s_builtAll = false;
            s_endWait = false;
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
            if (scope() == usb_queue_scope_cfg::Scope::All && beginScan(lv, c.index)) {  // [USB20a-2] скан у фоні; X уже грає
                publish();                                                               // building = true
            }
            finish(c.reqId, true, nullptr);
            break;
        }
        case CmdType::QNext: {
            if (s_building) {  // [USB20a-2]
                finish(c.reqId, false, usb_queue_scope_cfg::kWhyQueueBuilding);
                break;
            }
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
            if (s_building) {  // [USB20a-2]
                finish(c.reqId, false, usb_queue_scope_cfg::kWhyQueueBuilding);
                break;
            }
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
    if (shuf != s_appliedShuffle && !s_building) {  // [USB20a-2] під час скану shuffle застосується після нього (finishScan)
        s_appliedShuffle = shuf;
        rebuildForShuffle();
    }
    resumeTick(millis());  // [USB14]
    if (s_startReq.exchange(false)) {
        startPrepared();
    }
    libTick();   // [USB22a-1]
    scanTick();  // [USB20a-2]
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
        if (s_building) {  // [USB20a-2] черга ще добудовується: автоперехід чекає кінця скану (finishScan)
            s_endWait = true;
            Serial.printf("%s all: track ended while the queue is still building - waiting for the scan\n", kTag);
            return;
        }
        // Бібліотека «дограє» і сміттєвий файл без помилки: eof при позиції 0 і тривалості 0 = аудіо не було (файл < 1 с теж).
        if (AudioPlayer::fileDurationSec() == 0 && AudioPlayer::filePositionSec() == 0) {
            failAndSkip("no_audio");
            return;
        }
        onEnded();
        return;
    }
    if (s_endWait) {  // [USB20a-2] чекаємо кінця скану; «зовнішню зупинку» (Idle) не фіксуємо
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

// --- [USB22a-1] Бібліотека -----------------------------------------------------------------------------------------------------
const char* libStateName(LibState s) {
    switch (s) {
        case LibState::Building:
            return "building";
        case LibState::Ready:
            return "ready";
        case LibState::Empty:
            return "empty";
        case LibState::Error:
            return "error";
        default:
            return "none";
    }
}

bool libraryPage(uint16_t offset, uint16_t maxItems, uint32_t expectVersion, char* buf, size_t cap, LibPageInfo* out) {
    if (out == nullptr || buf == nullptr || cap < usb_library_cfg::kPageBufMin || !usb_nav::lock(usb_browser_cfg::kApiLockWaitMs)) {
        return false;
    }
    *out = LibPageInfo{};
    out->state = s_lib.state;
    out->version = s_lib.version;
    out->total = s_lib.nFiles;
    out->truncated = s_lib.truncated;
    out->reason = s_lib.reason;
    if (s_lib.state == LibState::Ready && s_all != nullptr) {
        if (expectVersion != 0 && expectVersion != s_lib.version) {
            out->stale = true;
        } else {
            size_t used = 0;
            uint16_t n = 0;
            uint16_t prevDir = kNoNode;
            size_t prevOff = 0;
            size_t prevLen = 0;
            for (uint32_t i = offset; i < s_lib.nFiles && n < maxItems; ++i) {
                const AllFile& f = s_all->files[i];
                const char* nm = s_all->pool + f.nameOff;
                const size_t nl = strlen(nm);
                const size_t dirOff = used + nl + 1;
                if (dirOff + 2 > cap) {
                    break;
                }
                size_t dl = 0;
                if (f.dirIdx == prevDir) {  // та сама папка, що в попередньому записі: копіюємо вже зібраний шлях
                    dl = prevLen;
                    if (dirOff + dl + 1 > cap) {
                        break;
                    }
                    memcpy(buf + dirOff, buf + prevOff, dl + 1);
                } else {
                    if (!allDirPath(f.dirIdx, buf + dirOff, cap - dirOff)) {
                        break;
                    }
                    dl = strlen(buf + dirOff);
                }
                memcpy(buf + used, nm, nl + 1);
                prevDir = f.dirIdx;
                prevOff = dirOff;
                prevLen = dl;
                used = dirOff + dl + 1;
                ++n;
            }
            out->returned = n;
        }
    }
    usb_nav::unlock();
    return true;
}

bool requestLibrary(const char** why) {
    if (why != nullptr) {
        *why = nullptr;
    }
    if (!usb_nav::modeOk()) {
        if (why != nullptr) {
            *why = "mode_not_ready";
        }
        return false;
    }
    const uint32_t now = millis();
    s_libReqAt.store(now == 0 ? 1u : now);
    // Задача usb_nav зазвичай уже працює (відновлення при вході). Якщо ні — будимо її читанням кореня (безпечно: список ще недійсний).
    // QStop для цього НЕ годиться: на відтворенні він зупинив би трек.
    usb_browser::Status bs;
    if (usb_browser::status(&bs) && !bs.active) {
        if (!usb_browser::requestRefresh(nullptr, why)) {
            s_libReqAt.store(0);
            return false;
        }
    }
    return true;
}

void setShuffle(bool on) { s_shuffle.store(on); }
bool shuffle() { return s_shuffle.load(); }
void setRepeatAll(bool on) { s_repeat.store(on); }
bool repeatAll() { return s_repeat.load(); }
void setScope(usb_queue_scope_cfg::Scope s) { s_scope.store(static_cast<uint8_t>(s)); }  // [USB20a-1]
usb_queue_scope_cfg::Scope scope() {
    return s_scope.load() == static_cast<uint8_t>(usb_queue_scope_cfg::Scope::All) ? usb_queue_scope_cfg::Scope::All
                                                                                   : usb_queue_scope_cfg::Scope::Folder;
}

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
    Serial.printf("%s stat: scope option=%s saved=%s built=%s truncated=%d building=%d\n", kTag,  // [USB20a-1]
                  usb_queue_scope_cfg::name(scope()), usb_queue_scope_cfg::name(SettingsStore::getPlayerScope()),
                  q.builtScope == 0 ? "-" : (q.builtScope == 1 ? "folder" : "all"), q.truncated ? 1 : 0, q.building ? 1 : 0);
    Serial.printf("%s stat: all: tables=%s builtAll=%d building=%d endWait=%d xOrdinal=%d files=%u dirs=%u pool=%u/%u B steps=%u busy=%u "
                  "skipped_dirs=%u truncated=%d\n",  // [USB20a-2]
                  kTag, s_all != nullptr ? "allocated" : "-", s_builtAll ? 1 : 0, s_building ? 1 : 0, s_endWait ? 1 : 0,
                  static_cast<int>(s_xOrdinal), s_all != nullptr ? static_cast<unsigned>(s_all->nFiles) : 0u,
                  s_all != nullptr ? static_cast<unsigned>(s_all->nDirs) : 0u,
                  s_all != nullptr ? static_cast<unsigned>(s_all->poolUsed) : 0u,
                  static_cast<unsigned>(usb_queue_all_cfg::kQueueAllPoolBytes), static_cast<unsigned>(s_scanSteps),
                  static_cast<unsigned>(s_scanBusy), static_cast<unsigned>(s_scanSkipped), s_allTruncated ? 1 : 0);
    {  // [USB22a-1]
        LibPub l = s_lib;  // діагностика: читаємо без lock (можлива неузгодженість лічильників не критична)
        Serial.printf("%s stat: library state=%s version=%lu files=%u dirs=%u truncated=%d scanActive=%d libWant=%d reason=%s\n", kTag,
                      libStateName(l.state), static_cast<unsigned long>(l.version), static_cast<unsigned>(l.nFiles),
                      static_cast<unsigned>(l.nDirs), l.truncated ? 1 : 0, s_scanActive ? 1 : 0, s_libWant ? 1 : 0,
                      l.reason != nullptr ? l.reason : "-");
    }
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
    } else if (word(rest, "scope", &arg)) {  // [USB20a-1] usb.q scope [folder|all]: RAM-дзеркало + NVS (як POST /api/usb/options)
        usb_queue_scope_cfg::Scope sc;
        if (arg[0] != '\0') {
            if (!usb_queue_scope_cfg::parse(arg, &sc)) {
                Serial.printf("%s usage: usb.q scope folder|all\n", kTag);
                return;
            }
            setScope(sc);
            SettingsStore::setPlayerScope(sc);
        }
        Serial.printf("%s scope option=%s saved=%s (applies to the NEXT play)\n", kTag, usb_queue_scope_cfg::name(scope()),
                      usb_queue_scope_cfg::name(SettingsStore::getPlayerScope()));
    } else if (word(rest, "scandelay", &arg)) {  // [USB20a-2] діагностика: usb.q scandelay <мс> (0 — вимкнути)
        if (arg[0] != '\0') {
            long v = atol(arg);
            if (v < 0) {
                v = 0;
            }
            if (v > 2000) {
                v = 2000;
            }
            s_scanDelayMs = static_cast<uint16_t>(v);
        }
        Serial.printf("%s scan delay per folder = %u ms (0 = off)\n", kTag, static_cast<unsigned>(s_scanDelayMs));
    } else {
        Serial.printf("%s usage: usb.q play <index> | next | prev | stop | stat | shuffle on|off | repeat on|off | scope [folder|all] | scandelay <ms>\n", kTag);
    }
}
#endif  // USB_HOST_DIAG

}  // namespace usb_queue

#endif  // FEATURE_USB_MODE
