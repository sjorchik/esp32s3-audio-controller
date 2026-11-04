// [USB10] Браузер папок флешки + фонова задача usb_nav. Див. usb_browser.h, usb_queue.h, config/usb_browser_config.h.
//
// Архітектура (відповіді на пункти 1, 4, 6a завдання):
//  - ОДНА задача usb_nav (ядро 0, пріоритет 1, стек usb_browser_cfg::kTaskStack) обслуговує і браузер, і чергу. Вона створюється за
//    першим запитом, поки режим «USB» у Ready / Playing, і видаляється, щойно режим вийшов із цих станів (usb_mode.cpp НЕ
//    змінено: задача сама опитує usb_mode::state()). Це не «нова задача на кожну дію»: одна, короткоживуча, лише в режимі.
//  - Читання каталогу й сортування — ЛИШЕ в цій задачі. Список будується в чорновому буфері (s_stage) без мьютекса й
//    підміняється з живим (s_live) під мьютексом на мікросекунди; публічні гетери копіюють дані під тим самим мьютексом із
//    таймаутом kApiLockWaitMs. Запити (request*) лише кладуть команду в статичну чергу FreeRTOS.
//  - Зберігання: імена UTF-8 у пулі + масив Entry (12 Б), 3 списки (живий, чорновий, черга) у PSRAM, виділяються один раз на
//    сеанс режиму (задача старт / стоп), тож фрагментації внутрішньої RAM немає; у внутрішній RAM лише Listing-заголовки
//    (≈ 0,3 КБ кожен), статичні мьютекс / черга і стек задачі.
//  - Покоління монтування: usb_msc_mount::volumeLock / volumeGenerationLocked (usb_fs.* не змінено). Будь-яка зміна покоління
//    робить список недійсним, зупиняє чергу й планує перечитування кореня.
//  - Осиротілі fs::File / каталоги: каталог відкривається лише на час одного readDir і закривається в тому ж виклику
//    (dir.close() на всіх шляхах); при вийманні посеред читання usb_fs повертає порожні результати, а fclose() на знятому томі
//    не викликається (відоме обмеження USB6: витік ≈ 4 КБ на осиротілий FILE) — дані для кроку f збирає hwcheck (usb.heap).
//
// Правило сортування (compareNames): спочатку папки, потім файли; усередині — порівняння за послідовністю кодових точок
// Unicode ПІСЛЯ згортання регістру: ASCII A–Z, Latin-1 À–Þ, кирилиця U+0400–U+042F, Ґ/ґ-пари U+0490–U+04BF. Український
// алфавіт впорядковано за абеткою (а б в г ґ д е є ж з и і ї й … я), тож «Їжак» стоїть між «Івано» і «Йосип». МЕЖІ: без
// «природного» числового порядку («track10» раніше «track2»); діакритика й латинські розширення (Ä, Ł, Ö…) регістр НЕ
// згортають (за кодовою точкою); російські ё / ы / э / ъ — після «я»; невалідний UTF-8 береться побайтово як Latin-1;
// однакові після згортання імена розрізняються побайтово (детермінованість).

#include "usb/usb_browser.h"

#if FEATURE_USB_MODE

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <atomic>

#include "config/usb_queue_config.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_fs.h"
#include "usb/usb_mode.h"
#include "usb/usb_msc_mount.h"
#include "usb/usb_nav_internal.h"

namespace usb_nav {

namespace {

constexpr const char* kTag = "[USBN]";

// --- Згортання регістру й порядок -----------------------------------------------------------------------------------
uint32_t decodeUtf8(const char*& p) {
    const uint8_t c = static_cast<uint8_t>(*p);
    auto cont = [](const char* q) { return (static_cast<uint8_t>(*q) & 0xC0) == 0x80; };
    if (c < 0x80) {
        ++p;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && cont(p + 1)) {
        const uint32_t cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (static_cast<uint8_t>(p[1]) & 0x3F);
        if (cp >= 0x80) {
            p += 2;
            return cp;
        }
    } else if ((c & 0xF0) == 0xE0 && cont(p + 1) && cont(p + 2)) {
        const uint32_t cp = (static_cast<uint32_t>(c & 0x0F) << 12) | (static_cast<uint32_t>(static_cast<uint8_t>(p[1]) & 0x3F) << 6) |
                            (static_cast<uint8_t>(p[2]) & 0x3F);
        if (cp >= 0x800) {
            p += 3;
            return cp;
        }
    } else if ((c & 0xF8) == 0xF0 && cont(p + 1) && cont(p + 2) && cont(p + 3)) {
        const uint32_t cp = (static_cast<uint32_t>(c & 0x07) << 18) | (static_cast<uint32_t>(static_cast<uint8_t>(p[1]) & 0x3F) << 12) |
                            (static_cast<uint32_t>(static_cast<uint8_t>(p[2]) & 0x3F) << 6) | (static_cast<uint8_t>(p[3]) & 0x3F);
        if (cp >= 0x10000) {
            p += 4;
            return cp;
        }
    }
    ++p;  // невалідна послідовність: байт як Latin-1
    return c;
}

uint32_t foldCp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp < 0x80) return cp;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;  // Latin-1: À–Þ (без ×)
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;            // А–Я
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;            // Ѐ–Џ (в т. ч. Є І Ї)
    if (cp >= 0x490 && cp <= 0x4BF && (cp & 1u) == 0) return cp + 1;  // Ґ та пари до U+04BF
    return cp;
}

// Український алфавіт (малі), 33 літери, у порядку абетки.
constexpr uint16_t kUk[33] = {0x430, 0x431, 0x432, 0x433, 0x491, 0x434, 0x435, 0x454, 0x436, 0x437, 0x438,
                              0x456, 0x457, 0x439, 0x43A, 0x43B, 0x43C, 0x43D, 0x43E, 0x43F, 0x440, 0x441,
                              0x442, 0x443, 0x444, 0x445, 0x446, 0x447, 0x448, 0x449, 0x44C, 0x44E, 0x44F};

// Ключ порядку: < 0x400 — як є; українська літера — 0x400 + номер в абетці; решта >= 0x400 — після української.
uint32_t collKey(uint32_t f) {
    if (f < 0x400) return f;
    for (uint32_t i = 0; i < 33; ++i) {
        if (kUk[i] == f) return 0x400 + i;
    }
    return f + 0x40;
}

int compareNames(const char* a, const char* b) {
    const char* pa = a;
    const char* pb = b;
    while (*pa != '\0' && *pb != '\0') {
        const uint32_t ka = collKey(foldCp(decodeUtf8(pa)));
        const uint32_t kb = collKey(foldCp(decodeUtf8(pb)));
        if (ka != kb) {
            return ka < kb ? -1 : 1;
        }
    }
    if (*pa == '\0' && *pb == '\0') {
        return strcmp(a, b);  // однакові після згортання: детермінований порядок
    }
    return (*pa == '\0') ? -1 : 1;  // коротший (префікс) першим
}

bool isHiddenName(const char* name) {
    if (name[0] == '.') {
        return true;
    }
    for (const char* s : usb_browser_cfg::kSkipNames) {
        if (strcasecmp(name, s) == 0) {
            return true;
        }
    }
    return false;
}

bool hasAudioExt(const char* name) {
    const char* dot = strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    for (const char* e : usb_browser_cfg::kAudioExtensions) {
        if (strcasecmp(dot, e) == 0) {
            return true;
        }
    }
    return false;
}

// --- Стан задачі / мьютекс / черга команд ----------------------------------------------------------------------------
Listing s_live;   // читають гетери (під мьютексом); пише лише задача usb_nav (під мьютексом)
Listing s_stage;  // чорновий; лише задача usb_nav

std::atomic<uint8_t> s_worker{0};
std::atomic<uint32_t> s_reqCounter{0};
std::atomic<uint32_t> s_version{0};
std::atomic<bool> s_inProgress{false};

StaticSemaphore_t s_muxBuf;
SemaphoreHandle_t s_mux = nullptr;
StaticQueue_t s_qBuf;
uint8_t s_qStorage[usb_browser_cfg::kCmdQueueLen * sizeof(Cmd)];
QueueHandle_t s_cmdQ = nullptr;
portMUX_TYPE s_initMux = portMUX_INITIALIZER_UNLOCKED;
bool s_initClaimed = false;
std::atomic<bool> s_initReady{false};

// Під мьютексом:
bool s_active = false;
uint32_t s_lastReqId = 0;
bool s_lastOk = false;
const char* s_lastWhy = nullptr;
const char* s_lastError = nullptr;

void initOnce() {
    if (s_initReady.load()) {
        return;
    }
    bool claim = false;
    portENTER_CRITICAL(&s_initMux);
    if (!s_initClaimed) {
        s_initClaimed = true;
        claim = true;
    }
    portEXIT_CRITICAL(&s_initMux);
    if (claim) {
        s_mux = xSemaphoreCreateMutexStatic(&s_muxBuf);
        s_cmdQ = xQueueCreateStatic(usb_browser_cfg::kCmdQueueLen, sizeof(Cmd), s_qStorage, &s_qBuf);
        s_initReady.store(true);
    } else {
        while (!s_initReady.load()) {
            vTaskDelay(1);
        }
    }
}

void setLastError(const char* e) {
    if (lock(UINT32_MAX)) {
        s_lastError = e;
        unlock();
    }
}

struct RamNow {
    uint32_t freeInt, largestInt, freePs;
};
RamNow ramNow() {
    RamNow r;
    r.freeInt = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    r.largestInt = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    r.freePs = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return r;
}

// Читає каталог у s_stage й публікує. reqId == 0 — внутрішнє читання (без finish()). true — опубліковано.
bool readAndPublish(const char* path, uint8_t depth, uint32_t reqId, const char* label) {
    const RamNow r0 = ramNow();
    const ReadResult r = readDir(path, depth, s_stage);
    const RamNow r1 = ramNow();
    if (r != ReadResult::Ok) {
        const char* why = readResultName(r);
        Serial.printf("%s %s '%s': FAILED (%s) after %u ms\n", kTag, label, path, why, static_cast<unsigned>(s_stage.readMs));
        setLastError(why);
        if (reqId != 0) {
            finish(reqId, false, why);
        }
        return false;
    }
    s_stage.version = s_version.fetch_add(1) + 1;
    lock(UINT32_MAX);
    const Listing tmp = s_live;
    s_live = s_stage;
    s_stage = tmp;
    s_lastError = nullptr;
    unlock();
    const Listing& l = s_live;
    Serial.printf("%s %s '%s': %u entries (dirs=%u files=%u)%s in %u ms | skipped hidden=%u other=%u long=%u | v=%u | "
                  "ram internal free=%u->%u largest=%u->%u psram_free=%u->%u%s\n",
                  kTag, label, l.cwd, static_cast<unsigned>(l.count), static_cast<unsigned>(l.dirs),
                  static_cast<unsigned>(l.count - l.dirs), l.truncated ? " TRUNCATED" : "", static_cast<unsigned>(l.readMs),
                  static_cast<unsigned>(l.skippedHidden), static_cast<unsigned>(l.skippedOther),
                  static_cast<unsigned>(l.skippedLong), static_cast<unsigned>(l.version), static_cast<unsigned>(r0.freeInt),
                  static_cast<unsigned>(r1.freeInt), static_cast<unsigned>(r0.largestInt), static_cast<unsigned>(r1.largestInt),
                  static_cast<unsigned>(r0.freePs), static_cast<unsigned>(r1.freePs),
                  l.readMs >= usb_browser_cfg::kRefreshWarnMs ? " (SLOW)" : "");
    if (reqId != 0) {
        finish(reqId, true, nullptr);
    }
    return true;
}

// Список недійсний (вийняття / перемонтування / вихід).
void invalidateLive(const char* why) {
    lock(UINT32_MAX);
    s_live.valid = false;
    s_live.count = 0;
    s_live.dirs = 0;
    s_live.poolUsed = 0;
    s_live.truncated = false;
    s_live.depth = 0;
    strcpy(s_live.cwd, "/");
    s_live.version = s_version.fetch_add(1) + 1;
    s_lastError = why;
    unlock();
}

void mediaTick(uint32_t* seenGen, bool* needRoot) {
    const uint32_t g = generation();
    if (g == kGenUnknown) {
        return;  // том зайнятий довше kGenLockWaitMs — спробуємо наступного циклу
    }
    if (g != *seenGen) {
        if (*seenGen != 0) {
            Serial.printf("%s media %s (generation %u -> %u): list invalidated, queue stopped\n", kTag,
                          g == 0 ? "removed" : "remounted", static_cast<unsigned>(*seenGen), static_cast<unsigned>(g));
            invalidateLive("media_changed");
            q::onMediaGone();
        }
        *seenGen = g;
        if (g != 0) {
            *needRoot = true;
        }
    }
    if (*needRoot && g != 0) {
        *needRoot = false;
        readAndPublish("/", 0, 0, "root");
    }
}

// Шлях підпапки name в cwd; false — задовгий.
bool joinPath(const char* cwd, const char* name, char* out, size_t cap) {
    const bool root = (cwd[0] == '/' && cwd[1] == '\0');
    const int n = snprintf(out, cap, "%s%s%s", cwd, root ? "" : "/", name);
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        return false;
    }
    return strlen(out) + strlen(usb_host_cfg::kMountPath) < usb_browser_cfg::kPathMax;
}

void handleCmd(const Cmd& c) {
    char path[usb_browser_cfg::kPathMax];
    switch (c.type) {
        case CmdType::Refresh: {
            const char* p = s_live.valid ? s_live.cwd : "/";
            const uint8_t d = s_live.valid ? s_live.depth : 0;
            strncpy(path, p, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            readAndPublish(path, d, c.reqId, "refresh");
            break;
        }
        case CmdType::EnterDir: {
            const char* why = nullptr;
            if (!s_live.valid) {
                why = "list_invalid";
            } else if (c.expectVersion != 0 && c.expectVersion != s_live.version) {
                why = "stale";
            } else if (c.index >= s_live.count) {
                why = "bad_index";
            } else if (!s_live.e[c.index].isDir) {
                why = "not_dir";
            } else if (s_live.depth + 1 > usb_browser_cfg::kMaxDepth) {
                why = "too_deep";
            } else if (!joinPath(s_live.cwd, nameAt(s_live, c.index), path, sizeof(path))) {
                why = "path_too_long";
            }
            if (why != nullptr) {
                Serial.printf("%s cd: refused (%s)\n", kTag, why);
                finish(c.reqId, false, why);
                break;
            }
            readAndPublish(path, static_cast<uint8_t>(s_live.depth + 1), c.reqId, "cd");
            break;
        }
        case CmdType::Up: {
            if (!s_live.valid) {
                Serial.printf("%s up: refused (list_invalid)\n", kTag);
                finish(c.reqId, false, "list_invalid");
                break;
            }
            if (s_live.depth == 0) {
                Serial.printf("%s up: refused (at_root)\n", kTag);
                finish(c.reqId, false, "at_root");
                break;
            }
            strncpy(path, s_live.cwd, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            char* slash = strrchr(path, '/');
            if (slash == nullptr || slash == path) {
                strcpy(path, "/");
            } else {
                *slash = '\0';
            }
            readAndPublish(path, static_cast<uint8_t>(s_live.depth - 1), c.reqId, "up");
            break;
        }
        case CmdType::QPlay:
        case CmdType::QNext:
        case CmdType::QPrev:
        case CmdType::QStop:
            q::handle(c);
            break;
    }
}

void workerMain(void*) {
    bool ok = allocListing(s_live) && allocListing(s_stage);
    if (ok) {
        ok = q::onWorkerStart();
    }
    if (!ok) {
        Serial.printf("%s worker: buffer allocation failed (PSRAM missing / full?) - browser unavailable\n", kTag);
        setLastError("no_psram");
    } else {
        lock(UINT32_MAX);
        s_active = true;
        s_lastError = nullptr;
        unlock();
        Serial.printf("%s worker: started (entries<=%u, pool=%u B, psram_free=%u)\n", kTag,
                      static_cast<unsigned>(usb_browser_cfg::kMaxEntries), static_cast<unsigned>(usb_browser_cfg::kNamePoolBytes),
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        uint32_t seenGen = 0;
        bool needRoot = false;
        uint32_t lastMedia = millis() - usb_browser_cfg::kMediaPollMs;  // перша перевірка одразу
        while (modeOk()) {
            if (static_cast<uint32_t>(millis() - lastMedia) >= usb_browser_cfg::kMediaPollMs) {
                lastMedia = millis();
                mediaTick(&seenGen, &needRoot);
            }
            Cmd c;
            while (modeOk() && xQueueReceive(s_cmdQ, &c, 0) == pdTRUE) {
                s_inProgress.store(true);
                handleCmd(c);
                s_inProgress.store(false);
            }
            q::tick();
            vTaskDelay(pdMS_TO_TICKS(usb_browser_cfg::kPollMs));
        }
    }
    q::onWorkerStop();
    lock(UINT32_MAX);
    s_active = false;
    freeListing(s_live);
    freeListing(s_stage);
    unlock();
    Serial.printf("%s worker: exit (stack min free=%u B)\n", kTag, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    s_worker.store(0);
    vTaskDelete(nullptr);
}

bool ensureWorker(const char** why) {
    initOnce();
    uint8_t expected = 0;
    if (s_worker.compare_exchange_strong(expected, 1)) {
        xQueueReset(s_cmdQ);
        s_inProgress.store(false);
        const BaseType_t ok = xTaskCreatePinnedToCore(workerMain, usb_browser_cfg::kTaskName, usb_browser_cfg::kTaskStack, nullptr,
                                                      usb_browser_cfg::kTaskPriority, nullptr, usb_browser_cfg::kTaskCore);
        if (ok != pdPASS) {
            s_worker.store(0);
            if (why != nullptr) {
                *why = "task_create_failed";
            }
            Serial.printf("%s worker: task create failed (low internal RAM?)\n", kTag);
            return false;
        }
    }
    return true;
}

}  // namespace

// --- Внутрішній API (usb_nav_internal.h) -----------------------------------------------------------------------------
const char* readResultName(ReadResult r) {
    switch (r) {
        case ReadResult::Ok:           return "ok";
        case ReadResult::NotMounted:   return "not_mounted";
        case ReadResult::VolumeBusy:   return "volume_busy";
        case ReadResult::OpenFailed:   return "open_failed";
        case ReadResult::NotDir:       return "not_dir";
        case ReadResult::MediaChanged: return "media_changed";
        case ReadResult::ModeGone:     return "mode_gone";
    }
    return "?";
}

bool allocListing(Listing& l) {
    memset(&l, 0, sizeof(l));
    l.cap = usb_browser_cfg::kMaxEntries;
    l.poolCap = usb_browser_cfg::kNamePoolBytes;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    l.e = static_cast<Entry*>(heap_caps_malloc(sizeof(Entry) * l.cap, caps));
    l.pool = static_cast<char*>(heap_caps_malloc(l.poolCap, caps));
    if (l.e == nullptr || l.pool == nullptr) {
        freeListing(l);
        return false;
    }
    strcpy(l.cwd, "/");
    return true;
}

void freeListing(Listing& l) {
    if (l.e != nullptr) {
        heap_caps_free(l.e);
    }
    if (l.pool != nullptr) {
        heap_caps_free(l.pool);
    }
    memset(&l, 0, sizeof(l));
    strcpy(l.cwd, "/");
}

void resetListing(Listing& l) {
    l.count = 0;
    l.dirs = 0;
    l.poolUsed = 0;
    l.truncated = false;
    l.valid = false;
    l.skippedHidden = 0;
    l.skippedOther = 0;
    l.skippedLong = 0;
    l.depth = 0;
    l.gen = 0;
    l.readMs = 0;
    strcpy(l.cwd, "/");
}

const char* nameAt(const Listing& l, uint16_t i) { return (l.pool != nullptr && i < l.count) ? l.pool + l.e[i].nameOff : ""; }

bool copyFiles(const Listing& src, Listing& dst) {
    resetListing(dst);
    strncpy(dst.cwd, src.cwd, sizeof(dst.cwd) - 1);
    dst.cwd[sizeof(dst.cwd) - 1] = '\0';
    dst.depth = src.depth;
    dst.gen = src.gen;
    dst.version = src.version;
    for (uint16_t i = 0; i < src.count; ++i) {
        const Entry& s = src.e[i];
        if (s.isDir) {
            continue;
        }
        if (dst.count >= dst.cap || dst.poolUsed + s.nameLen + 1u > dst.poolCap) {
            return false;
        }
        Entry& d = dst.e[dst.count++];
        d = s;
        d.nameOff = dst.poolUsed;
        memcpy(dst.pool + dst.poolUsed, src.pool + s.nameOff, s.nameLen + 1u);
        dst.poolUsed += s.nameLen + 1u;
    }
    dst.valid = true;
    return true;
}

uint32_t generation() {
    if (!usb_msc_mount::isMounted()) {
        return 0;
    }
    if (!usb_msc_mount::volumeLock(usb_browser_cfg::kGenLockWaitMs)) {
        return kGenUnknown;
    }
    const uint32_t g = usb_msc_mount::volumeGenerationLocked();
    usb_msc_mount::volumeUnlock();
    return g;
}

bool lock(uint32_t waitMs) {
    initOnce();
    return xSemaphoreTake(s_mux, waitMs == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void unlock() { xSemaphoreGive(s_mux); }

bool modeOk() {
    const usb_mode::State s = usb_mode::state();
    return s == usb_mode::State::Ready || s == usb_mode::State::Playing;
}

const Listing& live() { return s_live; }

void finish(uint32_t reqId, bool ok, const char* why) {
    lock(UINT32_MAX);
    s_lastReqId = reqId;
    s_lastOk = ok;
    s_lastWhy = why;
    unlock();
}

bool post(CmdType t, uint16_t index, uint32_t expectVersion, uint32_t* reqIdOut, const char** why) {
    if (why != nullptr) {
        *why = nullptr;
    }
    if (!modeOk()) {
        if (why != nullptr) {
            *why = "mode_not_ready";
        }
        return false;
    }
    if (!ensureWorker(why)) {
        return false;
    }
    Cmd c;
    c.type = t;
    c.index = index;
    c.expectVersion = expectVersion;
    c.reqId = s_reqCounter.fetch_add(1) + 1;
    if (xQueueSend(s_cmdQ, &c, 0) != pdTRUE) {
        if (why != nullptr) {
            *why = "queue_full";
        }
        return false;
    }
    if (reqIdOut != nullptr) {
        *reqIdOut = c.reqId;
    }
    return true;
}

ReadResult readDir(const char* path, uint8_t depth, Listing& out) {
    const uint32_t t0 = millis();
    const uint32_t gen0 = generation();
    if (gen0 == 0) {
        return ReadResult::NotMounted;
    }
    if (gen0 == kGenUnknown) {
        return ReadResult::VolumeBusy;
    }
    resetListing(out);
    strncpy(out.cwd, path, sizeof(out.cwd) - 1);
    out.cwd[sizeof(out.cwd) - 1] = '\0';
    out.depth = depth;

    fs::File dir = usb_fs::volume().open(path);
    if (!dir) {
        return ReadResult::OpenFailed;
    }
    if (!dir.isDirectory()) {
        dir.close();
        return ReadResult::NotDir;
    }
    const size_t cwdLen = strlen(out.cwd);
    const bool root = (cwdLen == 1);
    const size_t mountLen = strlen(usb_host_cfg::kMountPath);
    uint32_t seen = 0;
    ReadResult res = ReadResult::Ok;
    for (;;) {
        // getNextFileName() лише читає запис каталогу (readdir) БЕЗ відкриття файлу. openNextFile() відкривав кожен файл
        // за імʼям, а FAT шукає імʼя лінійно з початку каталогу: вимір USB10 — 28 записів 120 мс, 120 — 0,9 с, 500 — 9,9 с,
        // 1000 — 35 с (квадратично). Ціна: розмір файлу в списку недоступний (size = 0).
        bool isDir = false;
        const String full = dir.getNextFileName(&isDir);
        if (full.length() == 0) {
            break;
        }
        if ((++seen % usb_browser_cfg::kYieldEvery) == 0) {
            vTaskDelay(1);
            if (!modeOk()) {
                res = ReadResult::ModeGone;
                break;
            }
        }
        const char* base = full.c_str();
        const char* sl = strrchr(base, '/');
        if (sl != nullptr) {
            base = sl + 1;
        }
        if (base[0] == '\0') {
            continue;
        }
        const size_t n = strlen(base);
        if (isHiddenName(base)) {
            ++out.skippedHidden;
            continue;
        }
        if (!isDir && !hasAudioExt(base)) {
            ++out.skippedOther;
            continue;
        }
        if ((root ? 0 : cwdLen) + 1 + n + mountLen >= usb_browser_cfg::kPathMax) {
            ++out.skippedLong;
            continue;
        }
        if (out.count >= out.cap || out.poolUsed + n + 1 > out.poolCap) {
            out.truncated = true;
            break;
        }
        Entry& en = out.e[out.count++];
        en.nameOff = out.poolUsed;
        en.nameLen = static_cast<uint16_t>(n);
        en.isDir = isDir ? 1 : 0;
        en.reserved = 0;
        en.size = 0;
        memcpy(out.pool + out.poolUsed, base, n + 1);
        out.poolUsed += static_cast<uint32_t>(n + 1);
    }
    dir.close();
    if (res != ReadResult::Ok) {
        return res;
    }
    const uint32_t gen1 = generation();
    if (gen1 != gen0 && gen1 != kGenUnknown) {
        return ReadResult::MediaChanged;
    }
    const char* pool = out.pool;
    std::sort(out.e, out.e + out.count, [pool](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) {
            return a.isDir > b.isDir;
        }
        return compareNames(pool + a.nameOff, pool + b.nameOff) < 0;
    });
    uint16_t dirs = 0;
    while (dirs < out.count && out.e[dirs].isDir) {
        ++dirs;
    }
    out.dirs = dirs;
    out.gen = gen0;
    out.valid = true;
    out.readMs = static_cast<uint32_t>(millis() - t0);
    return ReadResult::Ok;
}

}  // namespace usb_nav

// ---------------------------------------------------------------------------------------------------------------------
// Публічний API
// ---------------------------------------------------------------------------------------------------------------------
namespace usb_browser {

bool status(Status* out) {
    if (out == nullptr) {
        return false;
    }
    if (!usb_nav::lock(usb_browser_cfg::kApiLockWaitMs)) {
        return false;
    }
    const usb_nav::Listing& l = usb_nav::s_live;
    *out = Status{};
    out->active = usb_nav::s_active;
    out->valid = usb_nav::s_active && l.valid;
    out->truncated = l.truncated;
    out->version = l.version;
    out->count = l.valid ? l.count : 0;
    out->dirs = l.valid ? l.dirs : 0;
    out->depth = l.depth;
    out->skippedHidden = l.skippedHidden;
    out->skippedOther = l.skippedOther;
    out->skippedLong = l.skippedLong;
    out->readMs = l.readMs;
    out->lastReqId = usb_nav::s_lastReqId;
    out->lastOk = usb_nav::s_lastOk;
    out->lastWhy = usb_nav::s_lastWhy;
    out->lastError = usb_nav::s_lastError;
    usb_nav::unlock();
    out->pending = static_cast<uint8_t>((usb_nav::s_cmdQ != nullptr ? uxQueueMessagesWaiting(usb_nav::s_cmdQ) : 0) +
                                        (usb_nav::s_inProgress.load() ? 1 : 0));
    return true;
}

bool cwd(char* out, size_t cap) {
    if (out == nullptr || cap == 0 || !usb_nav::lock(usb_browser_cfg::kApiLockWaitMs)) {
        return false;
    }
    const usb_nav::Listing& l = usb_nav::s_live;
    bool ok = false;
    if (usb_nav::s_active && l.valid && strlen(l.cwd) < cap) {
        strcpy(out, l.cwd);
        ok = true;
    }
    usb_nav::unlock();
    return ok;
}

bool entry(uint16_t index, char* nameOut, size_t nameCap, EntryInfo* info) {
    if (nameOut == nullptr || nameCap == 0 || !usb_nav::lock(usb_browser_cfg::kApiLockWaitMs)) {
        return false;
    }
    const usb_nav::Listing& l = usb_nav::s_live;
    bool ok = false;
    if (usb_nav::s_active && l.valid && l.e != nullptr && index < l.count) {
        const usb_nav::Entry& en = l.e[index];
        if (en.nameLen < nameCap) {
            memcpy(nameOut, l.pool + en.nameOff, en.nameLen + 1u);
            if (info != nullptr) {
                info->isDir = en.isDir != 0;
                info->size = en.size;
            }
            ok = true;
        }
    }
    usb_nav::unlock();
    return ok;
}

bool requestRefresh(uint32_t* reqId, const char** why) {
    return usb_nav::post(usb_nav::CmdType::Refresh, 0, 0, reqId, why);
}

bool requestEnterDir(uint16_t index, uint32_t expectVersion, uint32_t* reqId, const char** why) {
    return usb_nav::post(usb_nav::CmdType::EnterDir, index, expectVersion, reqId, why);
}

bool requestUp(uint32_t* reqId, const char** why) { return usb_nav::post(usb_nav::CmdType::Up, 0, 0, reqId, why); }

#if USB_HOST_DIAG
namespace {

constexpr const char* kTagC = "[USBN]";

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

bool waitReq(uint32_t reqId, Status* st) {
    const uint32_t t0 = millis();
    for (;;) {
        if (status(st) && st->lastReqId >= reqId) {
            return true;
        }
        if (static_cast<uint32_t>(millis() - t0) > usb_queue_cfg::kConsoleWaitMs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void printDir(long page) {
    Status st;
    if (!status(&st)) {
        Serial.printf("%s dir: busy, repeat\n", kTagC);
        return;
    }
    if (!st.active || !st.valid) {
        Serial.printf("%s dir: no list (active=%d valid=%d mode=%s media=%s last_error=%s) - 'usb.dir refresh' starts the browser\n",
                      kTagC, st.active ? 1 : 0, st.valid ? 1 : 0, usb_mode::stateName(usb_mode::state()),
                      usb_mode::mediaName(usb_mode::info().media), st.lastError != nullptr ? st.lastError : "-");
        return;
    }
    char path[usb_browser_cfg::kPathMax];
    if (!cwd(path, sizeof(path))) {
        strcpy(path, "?");
    }
    const unsigned per = static_cast<unsigned>(usb_host_cfg::kLsMaxLines);
    const unsigned pages = st.count == 0 ? 1u : (st.count + per - 1) / per;
    if (page < 1) page = 1;
    if (static_cast<unsigned long>(page) > pages) page = static_cast<long>(pages);
    Serial.printf("%s dir '%s' depth=%u: %u entries (dirs=%u files=%u)%s v=%lu read=%lu ms | skipped hidden=%u other=%u long=%u | page %ld/%u\n",
                  kTagC, path, static_cast<unsigned>(st.depth), static_cast<unsigned>(st.count), static_cast<unsigned>(st.dirs),
                  static_cast<unsigned>(st.count - st.dirs), st.truncated ? " TRUNCATED(kMaxEntries)" : "",
                  static_cast<unsigned long>(st.version), static_cast<unsigned long>(st.readMs),
                  static_cast<unsigned>(st.skippedHidden), static_cast<unsigned>(st.skippedOther),
                  static_cast<unsigned>(st.skippedLong), page, pages);
    char name[usb_browser_cfg::kPathMax];
    const unsigned first = static_cast<unsigned>(page - 1) * per;
    for (unsigned i = first; i < st.count && i < first + per; ++i) {
        EntryInfo ei = {};
        if (!entry(static_cast<uint16_t>(i), name, sizeof(name), &ei)) {
            Serial.printf("%s   [%4u] (unreadable: list changed or busy)\n", kTagC, i);
            continue;
        }
        if (ei.isDir) {
            Serial.printf("%s   [%4u] d %10s  %s/\n", kTagC, i, "", name);
        } else {
            Serial.printf("%s   [%4u] - %10lu  %s\n", kTagC, i, static_cast<unsigned long>(ei.size), name);
        }
    }
    if (page < static_cast<long>(pages)) {
        Serial.printf("%s   ... usb.dir %ld for the next page\n", kTagC, page + 1);
    }
}

void report(const char* what, bool accepted, uint32_t reqId, const char* why) {
    if (!accepted) {
        Serial.printf("%s %s: REFUSED (%s)\n", kTagC, what, why != nullptr ? why : "?");
        return;
    }
    Status st;
    if (!waitReq(reqId, &st)) {
        Serial.printf("%s %s: no result within %lu ms (request %lu still pending; 'usb.dir' shows the list)\n", kTagC, what,
                      static_cast<unsigned long>(usb_queue_cfg::kConsoleWaitMs), static_cast<unsigned long>(reqId));
        return;
    }
    if (st.lastReqId == reqId && !st.lastOk) {
        Serial.printf("%s %s: FAILED (%s)\n", kTagC, what, st.lastWhy != nullptr ? st.lastWhy : "?");
        return;
    }
    Serial.printf("%s %s: done\n", kTagC, what);
}

}  // namespace

bool isConsoleCommand(const char* line) {
    const char* rest = nullptr;
    return line != nullptr && (word(line, "usb.dir", &rest) || word(line, "usb.cd", &rest) || word(line, "usb.pwd", &rest));
}

void handleConsoleLine(const char* line) {
    const char* rest = nullptr;
    if (word(line, "usb.dir", &rest)) {
        if (strcmp(rest, "refresh") == 0) {
            uint32_t id = 0;
            const char* why = nullptr;
            const bool ok = requestRefresh(&id, &why);
            report("refresh", ok, id, why);
            printDir(1);
        } else if (rest[0] == '\0') {
            printDir(1);
        } else {
            printDir(atol(rest));
        }
    } else if (word(line, "usb.pwd", &rest)) {
        char path[usb_browser_cfg::kPathMax];
        Status st;
        if (cwd(path, sizeof(path)) && status(&st)) {
            Serial.printf("%s pwd: '%s' depth=%u v=%lu\n", kTagC, path, static_cast<unsigned>(st.depth), static_cast<unsigned long>(st.version));
        } else {
            Serial.printf("%s pwd: no list (usb.dir refresh)\n", kTagC);
        }
    } else if (word(line, "usb.cd", &rest)) {
        uint32_t id = 0;
        const char* why = nullptr;
        bool ok = false;
        if (strcmp(rest, "..") == 0) {
            ok = requestUp(&id, &why);
            report("cd ..", ok, id, why);
        } else if (rest[0] >= '0' && rest[0] <= '9') {
            const long idx = atol(rest);
            ok = requestEnterDir(static_cast<uint16_t>(idx), 0, &id, &why);
            report("cd", ok, id, why);
        } else {
            Serial.printf("%s usage: usb.cd <index|..>\n", kTagC);
            return;
        }
        printDir(1);
    }
}
#endif  // USB_HOST_DIAG

}  // namespace usb_browser

#endif  // FEATURE_USB_MODE