// [DEF5a] Карта внутрішньої кучі (див. usb_heapmap.h і config/usb_heapmap_config.h). Лише USB_HEAPMAP = 1.
//
// Знімок: heap_caps_walk(MALLOC_CAP_INTERNAL) -> Rec{addr, size | used-біт} у PSRAM (callback нічого не друкує й не
// виділяє: він виконується під блокуванням кучі), далі сортування за адресою й порівняння двох відсортованих знімків
// злиттям. Усі буфери — одна арена в PSRAM, що виділяється ОДИН раз (внутрішня RAM не займається). Друк — через
// статичний буфер і Serial.println(const char*) (без malloc, на відміну від довгого Serial.printf).

#include "usb/usb_heapmap.h"

#if USB_HEAPMAP

#include <Arduino.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <atomic>
#include <utility>

#include "audio/audio_player.h"
#include "config/file_player_config.h"
#include "config/usb_heapmap_config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_memdiag.h"
#include "usb/usb_mode.h"
#include "usb/usb_msc_mount.h"

namespace usb_heapmap {

namespace {

using namespace usb_heapmap_cfg;

constexpr const char* kTag = "[HM]";
constexpr uint32_t kUsedBit = 0x80000000u;

struct Rec {
    uint32_t addr;
    uint32_t szUsed;  // розмір | kUsedBit (блок використано)
};
static_assert(sizeof(Rec) == 8, "usb_heapmap: Rec must be 8 bytes");

struct FreeTop {
    uint32_t addr;
    uint32_t size;
};

struct Slot {
    Rec* recs = nullptr;
    uint32_t n = 0;       // записів у recs
    bool trunc = false;   // блоків було більше за kHeapMapMaxBlocks
    bool valid = false;
    int8_t point = -1;    // 0..7 = C0..C7, -1 — ручний знімок
    uint32_t cycle = 0;
    char tag[16] = {0};
    uint32_t tMs = 0;
    uint32_t walkUs = 0;
    uint32_t sortUs = 0;
    uint32_t totalBlocks = 0;  // скільки блоків побачив обхід (≥ n при trunc)
    uint32_t freeBytes = 0;
    uint32_t largest = 0;
    uint32_t largestAddr = 0;
    uint32_t usedBlk = 0;
    uint32_t freeBlk = 0;
    uint32_t usedBytes = 0;
    FreeTop top[kTopFreeBlocks] = {};
};

struct TaskRec {
    uint32_t stackBase;
    uint32_t tcb;
    char name[configMAX_TASK_NAME_LEN];
    uint32_t seenScan;  // номер скану, у якому задачу бачили востаннє
};

struct Cand {
    uint32_t addr;
    uint32_t size;
};

// --- Стан (під s_mtx, окрім атомарних) ----------------------------------------------------------------------------
StaticSemaphore_t s_mtxBuf;
SemaphoreHandle_t s_mtx = nullptr;
std::atomic<int> s_initState{0};  // 0 — не ініціалізовано, 1 — ініціалізує хтось, 2 — готово

uint8_t* s_arena = nullptr;
bool s_arenaFailed = false;
Slot s_slots[3];
Slot* s_cur = &s_slots[0];   // сюди робиться новий знімок
Slot* s_prev = &s_slots[1];  // остання зафіксована точка
Slot* s_ref = &s_slots[2];   // еталон C1 останнього циклу
bool s_prevValid = false;
bool s_olderValid = false;  // у s_cur лежить знімок, що передував s_prev (для `usb.hm diff`)
bool s_refValid = false;
uint32_t s_refCycle = 0;
Cand* s_cand = nullptr;
TaskRec* s_tasks = nullptr;
TaskStatus_t* s_scan = nullptr;
uint32_t s_scanNo = 0;

std::atomic<bool> s_autoOn{true};
bool s_skipC0 = false;
uint32_t s_cycle = 0;
int8_t s_lastPoint = -1;
bool s_postExit = false;   // після C5, до успішного підключення радіо
bool s_c6Open = false;     // C6 знято, очікується результат
uint32_t s_c6EndMs = 0;
uint32_t s_retry = 0;
uint32_t s_failSeqAtCycle = 0;
std::atomic<uint32_t> s_resultSeq{0};   // скільки результатів C7 побачено (для usb.hm cycle)
std::atomic<int> s_lastResultOk{-1};    // -1 — не було; 0 — збій; 1 — успіх
std::atomic<uint32_t> s_lastResultMs{0};
uint32_t s_pointsTaken = 0;

char s_line[208];  // рядок виводу (під s_mtx або в консолі loopTask)

const char* const kPointName[8] = {"radio-playing", "radio-stopped", "mounted",    "file-playing",
                                   "file-stopped",  "host-removed",  "pre-connect", "connect-result"};

// --- Допоміжне -----------------------------------------------------------------------------------------------------
void emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);
    Serial.println(s_line);
}

bool initMutex() {
    int expected = 0;
    if (s_initState.compare_exchange_strong(expected, 1)) {
        s_mtx = xSemaphoreCreateMutexStatic(&s_mtxBuf);
        s_initState.store(2);
    }
    while (s_initState.load() != 2) {
        vTaskDelay(1);
    }
    return s_mtx != nullptr;
}

bool lock() {
    if (!initMutex()) {
        return false;
    }
    return xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) == pdTRUE;
}

void unlock() { xSemaphoreGive(s_mtx); }

// Арена в PSRAM: 3 знімки + кандидати + реєстр задач + масив uxTaskGetSystemState. Виділяється один раз.
bool ensureArena() {
    if (s_arena != nullptr) {
        return true;
    }
    if (s_arenaFailed) {
        return false;
    }
    const size_t recBytes = static_cast<size_t>(kHeapMapMaxBlocks) * sizeof(Rec);
    const size_t candBytes = static_cast<size_t>(kCandidateMax) * sizeof(Cand);
    const size_t taskBytes = static_cast<size_t>(kTaskRegistryMax) * sizeof(TaskRec);
    const size_t scanBytes = static_cast<size_t>(kTaskScanMax) * sizeof(TaskStatus_t);
    const size_t total = 3 * recBytes + candBytes + taskBytes + scanBytes + 64;
    uint8_t* a = static_cast<uint8_t*>(heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (a == nullptr) {
        s_arenaFailed = true;
        Serial.printf("%s ERROR: PSRAM arena (%u B) not allocated - usb_heapmap disabled\n", kTag,
                      static_cast<unsigned>(total));
        return false;
    }
    s_arena = a;
    uint8_t* p = (uint8_t*)(((uintptr_t)a + 7u) & ~(uintptr_t)7u);
    for (int i = 0; i < 3; ++i) {
        s_slots[i].recs = reinterpret_cast<Rec*>(p);
        p += recBytes;
    }
    s_cand = reinterpret_cast<Cand*>(p);
    p += candBytes;
    s_tasks = reinterpret_cast<TaskRec*>(p);
    p += taskBytes;
    s_scan = reinterpret_cast<TaskStatus_t*>(p);
    Serial.printf("%s arena in PSRAM: %u B (3 x %u records of 8 B); internal RAM is not used by the snapshots\n", kTag,
                  static_cast<unsigned>(total), static_cast<unsigned>(kHeapMapMaxBlocks));
    return true;
}

// --- Обхід кучі ----------------------------------------------------------------------------------------------------
struct WalkCtx {
    Rec* recs;
    uint32_t cap;
    uint32_t n;
    uint32_t total;
    uint32_t freeBytes;
    uint32_t largest;
    uint32_t largestAddr;
    uint32_t usedBlk;
    uint32_t freeBlk;
    uint32_t usedBytes;
};

// ВИКОНУЄТЬСЯ ПІД БЛОКУВАННЯМ КУПИ: лише арифметика й запис у PSRAM; без друку, виділень і мʼютексів.
bool walkCb(walker_heap_into_t /*heapInfo*/, walker_block_info_t b, void* user) {
    WalkCtx* c = static_cast<WalkCtx*>(user);
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b.ptr));
    const uint32_t size = static_cast<uint32_t>(b.size);
    c->total++;
    if (b.used) {
        c->usedBlk++;
        c->usedBytes += size;
    } else {
        c->freeBlk++;
        c->freeBytes += size;
        if (size > c->largest) {
            c->largest = size;
            c->largestAddr = addr;
        }
    }
    if (c->n < c->cap) {
        c->recs[c->n].addr = addr;
        c->recs[c->n].szUsed = (size & ~kUsedBit) | (b.used ? kUsedBit : 0u);
        c->n++;
    }
    return true;
}

int cmpRec(const void* a, const void* b) {
    const uint32_t x = static_cast<const Rec*>(a)->addr;
    const uint32_t y = static_cast<const Rec*>(b)->addr;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

void walkInto(Slot& s, int8_t point, const char* tag) {
    WalkCtx c = {s.recs, kHeapMapMaxBlocks, 0, 0, 0, 0, 0, 0, 0, 0};
    const int64_t t0 = esp_timer_get_time();
    heap_caps_walk(MALLOC_CAP_INTERNAL, walkCb, &c);
    const int64_t t1 = esp_timer_get_time();
    qsort(s.recs, c.n, sizeof(Rec), cmpRec);
    const int64_t t2 = esp_timer_get_time();
    s.n = c.n;
    s.trunc = c.total > c.n;
    s.valid = true;
    s.point = point;
    s.cycle = s_cycle;
    strncpy(s.tag, tag != nullptr ? tag : "", sizeof(s.tag) - 1);
    s.tag[sizeof(s.tag) - 1] = '\0';
    s.tMs = millis();
    s.walkUs = static_cast<uint32_t>(t1 - t0);
    s.sortUs = static_cast<uint32_t>(t2 - t1);
    s.totalBlocks = c.total;
    s.freeBytes = c.freeBytes;
    s.largest = c.largest;
    s.largestAddr = c.largestAddr;
    s.usedBlk = c.usedBlk;
    s.freeBlk = c.freeBlk;
    s.usedBytes = c.usedBytes;
    for (uint32_t k = 0; k < kTopFreeBlocks; ++k) {
        s.top[k] = {0, 0};
    }
    for (uint32_t i = 0; i < s.n; ++i) {  // три найбільші вільні (за записами; при trunc — наближено)
        if ((s.recs[i].szUsed & kUsedBit) != 0) {
            continue;
        }
        FreeTop cand = {s.recs[i].addr, s.recs[i].szUsed & ~kUsedBit};
        for (uint32_t k = 0; k < kTopFreeBlocks; ++k) {
            if (cand.size > s.top[k].size) {
                std::swap(cand, s.top[k]);
            }
        }
    }
}

// --- Реєстр задач (мітки блоків) ----------------------------------------------------------------------------------
void scanTasks() {
    ++s_scanNo;
#if (configUSE_TRACE_FACILITY == 1)
    const UBaseType_t n = uxTaskGetSystemState(s_scan, kTaskScanMax, nullptr);
    for (UBaseType_t i = 0; i < n; ++i) {
        const TaskStatus_t& ts = s_scan[i];
        const uint32_t sb = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ts.pxStackBase));
        const uint32_t tcb = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ts.xHandle));
        TaskRec* slot = nullptr;
        TaskRec* oldest = &s_tasks[0];
        for (uint32_t k = 0; k < kTaskRegistryMax; ++k) {
            TaskRec& r = s_tasks[k];
            if (r.stackBase == sb && r.tcb == tcb && r.tcb != 0) {
                slot = &r;
                break;
            }
            if (r.seenScan < oldest->seenScan) {
                oldest = &r;
            }
        }
        if (slot == nullptr) {
            slot = oldest;  // вільний (seenScan = 0) або найдавніше бачений
            slot->stackBase = sb;
            slot->tcb = tcb;
            strncpy(slot->name, ts.pcTaskName != nullptr ? ts.pcTaskName : "?", sizeof(slot->name) - 1);
            slot->name[sizeof(slot->name) - 1] = '\0';
        }
        slot->seenScan = s_scanNo;
    }
#endif
}

// Мітка блока: (а) стек / TCB задачі з реєстру, (б) збіг розміру з відомими константами (ПІДКАЗКА), (в) «?».
void label(uint32_t addr, uint32_t size, char* out, size_t cap) {
    for (uint32_t k = 0; k < kTaskRegistryMax; ++k) {
        const TaskRec& r = s_tasks[k];
        if (r.tcb == 0) {
            continue;
        }
        const bool alive = (r.seenScan == s_scanNo);
        if (r.stackBase == addr) {
            snprintf(out, cap, "stack:%s%s", r.name, alive ? "" : "(task gone!)");
            return;
        }
        if (r.tcb == addr) {
            snprintf(out, cap, "tcb:%s%s", r.name, alive ? "" : "(task gone!)");
            return;
        }
    }
    size_t off = 0;
    out[0] = '\0';
    bool anyStack = false;
    for (size_t k = 0; k < kKnownSizeCount; ++k) {
        if (kKnownSizes[k].size == size) {
            const int w = snprintf(out + off, cap - off, "%s%s", off != 0 ? "|" : "", kKnownSizes[k].what);
            if (w > 0) {
                off += static_cast<size_t>(w);
                if (off >= cap) {
                    off = cap - 1;
                }
            }
            anyStack = anyStack || kKnownSizes[k].isStack;
        }
    }
    if (off == 0) {
        snprintf(out, cap, "?");
        return;
    }
    // префікс «hint» — це збіг лише за розміром, не атрибуція
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", out);
    snprintf(out, cap, "hint%s:%s", anyStack ? "(stack?)" : "", tmp);
}

// --- Діф ----------------------------------------------------------------------------------------------------------
struct DiffStat {
    uint32_t newBlk, newBytes, bigNew, smallBlk, smallBytes, freedBlk, freedBytes, candCount;
};

DiffStat computeDiff(const Slot& cur, const Slot& ref) {
    DiffStat d = {};
    uint32_t j = 0;
    for (uint32_t i = 0; i < cur.n; ++i) {  // нові: використані в cur, яких не було (за адресою + розміром) у ref
        const Rec& r = cur.recs[i];
        if ((r.szUsed & kUsedBit) == 0) {
            continue;
        }
        const uint32_t sz = r.szUsed & ~kUsedBit;
        while (j < ref.n && ref.recs[j].addr < r.addr) {
            ++j;
        }
        if (j < ref.n && ref.recs[j].addr == r.addr && ref.recs[j].szUsed == r.szUsed) {
            continue;
        }
        d.newBlk++;
        d.newBytes += sz;
        if (sz >= kSmallBlockBytes) {
            d.bigNew++;
            if (d.candCount < kCandidateMax) {
                s_cand[d.candCount++] = {r.addr, sz};
            }
        } else {
            d.smallBlk++;
            d.smallBytes += sz;
        }
    }
    j = 0;
    for (uint32_t i = 0; i < ref.n; ++i) {  // зникли: використані в ref, яких немає (так само) в cur
        const Rec& r = ref.recs[i];
        if ((r.szUsed & kUsedBit) == 0) {
            continue;
        }
        while (j < cur.n && cur.recs[j].addr < r.addr) {
            ++j;
        }
        if (j < cur.n && cur.recs[j].addr == r.addr && cur.recs[j].szUsed == r.szUsed) {
            continue;
        }
        d.freedBlk++;
        d.freedBytes += r.szUsed & ~kUsedBit;
    }
    return d;
}

const char* refName(const Slot& s, char* buf, size_t cap) {
    if (s.point >= 0) {
        snprintf(buf, cap, "c%u.C%d", static_cast<unsigned>(s.cycle), static_cast<int>(s.point));
    } else {
        snprintf(buf, cap, "'%s'", s.tag);
    }
    return buf;
}

// maxLines — скільки НОВИХ блоків ≥ kSmallBlockBytes друкувати (найбільші першими). persistent — заголовок «ПОСТІЙНІ».
void printDiff(const Slot& cur, const Slot& ref, const char* what, uint32_t maxLines, bool persistent) {
    const DiffStat d = computeDiff(cur, ref);
    char rn[24];
    emit("%s c%u %s %s vs %s: +%u blk/%u B (>=%u B: %u | <%u B: %u blk/%u B) -%u blk/%u B%s", kTag,
         static_cast<unsigned>(cur.cycle), cur.point >= 0 ? kPointName[cur.point] : cur.tag,
         persistent ? "PERSISTENT-NEW" : "diff", what != nullptr ? what : refName(ref, rn, sizeof(rn)),
         static_cast<unsigned>(d.newBlk), static_cast<unsigned>(d.newBytes), static_cast<unsigned>(kSmallBlockBytes),
         static_cast<unsigned>(d.bigNew), static_cast<unsigned>(kSmallBlockBytes), static_cast<unsigned>(d.smallBlk),
         static_cast<unsigned>(d.smallBytes), static_cast<unsigned>(d.freedBlk), static_cast<unsigned>(d.freedBytes),
         (cur.trunc || ref.trunc) ? " [INCOMPLETE: snapshot truncated]" : "");
    uint32_t printed = 0;
    while (printed < maxLines && printed < d.candCount) {
        uint32_t best = 0;
        for (uint32_t k = 1; k < d.candCount; ++k) {
            if (s_cand[k].size > s_cand[best].size) {
                best = k;
            }
        }
        if (s_cand[best].size == 0) {
            break;
        }
        const Cand c = s_cand[best];
        s_cand[best].size = 0;
        char lb[72];
        label(c.addr, c.size, lb, sizeof(lb));
        const bool inLargest = ref.largest != 0 && c.addr >= ref.largestAddr && c.addr < ref.largestAddr + ref.largest;
        emit("%s    + 0x%08x size=%u %s%s", kTag, static_cast<unsigned>(c.addr), static_cast<unsigned>(c.size), lb,
             inLargest ? "  <-- INSIDE the largest free block of the reference" : "");
        ++printed;
    }
    if (d.bigNew > printed) {
        emit("%s    ... +%u more blocks >= %u B", kTag, static_cast<unsigned>(d.bigNew - printed),
             static_cast<unsigned>(kSmallBlockBytes));
    }
}

void printHeader(const Slot& s, const char* extra) {
    emit("%s c%u C%d %s%s%s t=%u INT free=%u largest=%u used=%ublk/%uB free_blk=%u walk=%uus sort=%uus recs=%u%s stk=%u",
         kTag, static_cast<unsigned>(s.cycle), static_cast<int>(s.point), s.point >= 0 ? kPointName[s.point] : s.tag,
         extra != nullptr ? " " : "", extra != nullptr ? extra : "", static_cast<unsigned>(s.tMs),
         static_cast<unsigned>(s.freeBytes), static_cast<unsigned>(s.largest), static_cast<unsigned>(s.usedBlk),
         static_cast<unsigned>(s.usedBytes), static_cast<unsigned>(s.freeBlk), static_cast<unsigned>(s.walkUs),
         static_cast<unsigned>(s.sortUs), static_cast<unsigned>(s.n), s.trunc ? " TRUNCATED" : "",
         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    size_t off = 0;
    int w = snprintf(s_line, sizeof(s_line), "%s c%u C%d top-free:", kTag, static_cast<unsigned>(s.cycle),
                     static_cast<int>(s.point));
    off = (w > 0) ? static_cast<size_t>(w) : 0;
    for (uint32_t k = 0; k < kTopFreeBlocks && off < sizeof(s_line); ++k) {
        w = snprintf(s_line + off, sizeof(s_line) - off, " 0x%08x/%u", static_cast<unsigned>(s.top[k].addr),
                     static_cast<unsigned>(s.top[k].size));
        if (w > 0) {
            off += static_cast<size_t>(w);
        }
    }
    Serial.println(s_line);
}

void saveRef(const Slot& from) {
    Rec* keep = s_ref->recs;
    *s_ref = from;
    s_ref->recs = keep;
    memcpy(keep, from.recs, static_cast<size_t>(from.n) * sizeof(Rec));
    s_refValid = true;
    s_refCycle = from.cycle;
}

void newCycle() {
    ++s_cycle;
    s_postExit = false;
    s_c6Open = false;
    s_retry = 0;
    s_failSeqAtCycle = usb_memdiag::failSeq();
}

// Блок «FAIL» (перед знімком C7 при збої): записи хука за цикл + знімок за класами (largest по INTERNAL / I8 / DMA / PSRAM).
void printFailBlock(uint32_t connMs) {
    emit("%s c%u FAIL radio connect failed after %u ms (see hook records and snapshot below)", kTag,
         static_cast<unsigned>(s_cycle), static_cast<unsigned>(connMs));
    static usb_memdiag::FailEntry ent[12];
    const uint32_t seqNow = usb_memdiag::failSeq();
    const uint32_t total = seqNow - s_failSeqAtCycle;
    const uint32_t got = usb_memdiag::failCopySince(s_failSeqAtCycle, ent, 12);
    emit("%s c%u FAIL hook records this cycle: %u (shown last %u)", kTag, static_cast<unsigned>(s_cycle),
         static_cast<unsigned>(total), static_cast<unsigned>(got));
    for (uint32_t k = 0; k < got; ++k) {
        char caps[48];
        usb_memdiag::capsToText(ent[k].caps, caps, sizeof(caps));
        emit("%s   hook#%u t=%u size=%u caps=0x%x (%s) fn=%s", kTag, static_cast<unsigned>(ent[k].seq),
             static_cast<unsigned>(ent[k].tMs), static_cast<unsigned>(ent[k].size), static_cast<unsigned>(ent[k].caps), caps,
             ent[k].fn);
    }
    usb_memdiag::print("hm:FAIL classes", usb_memdiag::take());
}

struct PointInfo {
    bool manual;
    int8_t p;
    const char* tag;
    bool hasResult;
    bool ok;
    uint32_t connMs;
};

// Знімає точку. Викликається ПІД s_mtx.
void takePoint(const PointInfo& pi) {
    if (!ensureArena()) {
        return;
    }
    usb_memdiag::installFailHook();  // ідемпотентно: один хук на систему
    if (!pi.manual) {
        switch (pi.p) {
            case 0:  // C0 — початок циклу
                if (s_skipC0) {
                    return;
                }
                newCycle();
                break;
            case 1:  // C1 — початок циклу, якщо C0 пропущено
                if (s_lastPoint != 0 || s_cycle == 0) {
                    newCycle();
                }
                break;
            case 6:  // C6 — лише після виходу, обмежена кількість повторів
                if (!s_postExit || s_retry >= kRetryPointsPerCycle) {
                    return;
                }
                break;
            default:
                if (s_cycle == 0) {
                    newCycle();
                }
                break;
        }
    }
    if (pi.hasResult && !pi.ok) {
        printFailBlock(pi.connMs);
    }
    const char* tag = pi.manual ? pi.tag : kPointName[pi.p];
    walkInto(*s_cur, pi.manual ? -1 : pi.p, tag);
    scanTasks();
    ++s_pointsTaken;

    char extra[40] = {0};
    if (pi.hasResult) {
        snprintf(extra, sizeof(extra), "%s connect_ms=%u", pi.ok ? "OK" : "FAIL", static_cast<unsigned>(pi.connMs));
    }
    printHeader(*s_cur, extra[0] != '\0' ? extra : nullptr);

    if (!pi.manual && pi.p == 0 && (s_cur->walkUs / 1000u) > kC0WalkMaxMs) {
        s_skipC0 = true;
        emit("%s WARNING: C0 walk took %u us (> %u ms) with the radio playing - C0 is skipped in the next cycles", kTag,
             static_cast<unsigned>(s_cur->walkUs), static_cast<unsigned>(kC0WalkMaxMs));
    }

    // Діф до попередньої точки (на C0 — не потрібен).
    if (s_prevValid && (pi.manual || pi.p != 0)) {
        printDiff(*s_cur, *s_prev, nullptr, kDiffLinesPerPoint, false);
    }
    if (!pi.manual && pi.p == 1) {  // C1 проти C1 попереднього циклу — найважливіший вивід
        if (s_refValid && s_refCycle != s_cycle) {
            char nm[32];
            snprintf(nm, sizeof(nm), "C1 of cycle %u", static_cast<unsigned>(s_refCycle));
            printDiff(*s_cur, *s_ref, nm, kPersistentLines, true);
        } else {
            emit("%s c%u C1 no reference yet (first cycle): this C1 is saved as the reference", kTag,
                 static_cast<unsigned>(s_cycle));
        }
        saveRef(*s_cur);
    } else if (!pi.manual && pi.p == 5 && s_refValid && s_refCycle == s_cycle) {  // після end(): що пережило C1 цього ж циклу
        printDiff(*s_cur, *s_ref, "C1 of THIS cycle", kPersistentLines, true);
    }

    std::swap(s_cur, s_prev);  // тепер s_prev = щойно знята точка, s_cur = попередня (для `usb.hm diff`)
    s_prevValid = true;
    s_olderValid = true;
    if (!pi.manual) {
        s_lastPoint = pi.p;
        if (pi.p == 5) {
            s_postExit = true;
            s_retry = 0;
        } else if (pi.p == 6) {
            s_c6Open = true;
            s_c6EndMs = millis();  // кінець C6: від нього рахується «мс до результату»
        } else if (pi.p == 7) {
            s_c6Open = false;
            ++s_retry;
            if (pi.ok) {
                s_postExit = false;
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Публічний API
// ---------------------------------------------------------------------------------------------------------------------
void point(Point p) {
    if (!s_autoOn.load()) {
        return;
    }
    if (!lock()) {
        return;
    }
    const PointInfo pi = {false, static_cast<int8_t>(p), nullptr, false, false, 0};
    takePoint(pi);
    unlock();
}

void connectResult(bool ok) {
    if (!s_autoOn.load()) {
        return;
    }
    if (!lock()) {
        return;
    }
    if (s_c6Open) {
        const uint32_t ms = millis() - s_c6EndMs;
        const PointInfo pi = {false, 7, nullptr, true, ok, ms};
        takePoint(pi);
        s_lastResultMs.store(ms);
        s_lastResultOk.store(ok ? 1 : 0);
        s_resultSeq.fetch_add(1);
    }
    unlock();
}

// ---------------------------------------------------------------------------------------------------------------------
// Консоль
// ---------------------------------------------------------------------------------------------------------------------
namespace {

const char* skipWs(const char* s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

bool word(const char* s, const char* w, const char** rest) {
    const size_t n = strlen(w);
    if (strncmp(s, w, n) != 0 || (s[n] != '\0' && s[n] != ' ' && s[n] != '\t')) {
        return false;
    }
    *rest = skipWs(s + n);
    return true;
}

void cmdStat() {
    Serial.printf("%s stat: auto=%s arena=%s skipC0=%d cycle=%u points=%u post_exit=%d c6_open=%d retry=%u ref=%s(cycle %u)\n",
                  kTag, s_autoOn.load() ? "on" : "off", s_arena != nullptr ? "PSRAM-ok" : (s_arenaFailed ? "FAILED" : "not-yet"),
                  s_skipC0 ? 1 : 0, static_cast<unsigned>(s_cycle), static_cast<unsigned>(s_pointsTaken), s_postExit ? 1 : 0,
                  s_c6Open ? 1 : 0, static_cast<unsigned>(s_retry), s_refValid ? "yes" : "no", static_cast<unsigned>(s_refCycle));
    Serial.printf("%s stat: last point c%u C%d '%s' walk=%uus sort=%uus recs=%u%s | hook records total=%u | connect results seen=%u last=%s %ums\n",
                  kTag, static_cast<unsigned>(s_prev->cycle), static_cast<int>(s_prev->point), s_prev->tag,
                  static_cast<unsigned>(s_prev->walkUs), static_cast<unsigned>(s_prev->sortUs), static_cast<unsigned>(s_prev->n),
                  s_prev->trunc ? " TRUNCATED" : "", static_cast<unsigned>(usb_memdiag::failSeq()),
                  static_cast<unsigned>(s_resultSeq.load()),
                  s_lastResultOk.load() < 0 ? "-" : (s_lastResultOk.load() ? "OK" : "FAIL"),
                  static_cast<unsigned>(s_lastResultMs.load()));
}

void cmdSnap(const char* tagArg) {
    char tag[16];
    size_t n = 0;
    while (tagArg[n] != '\0' && tagArg[n] != ' ' && tagArg[n] != '\t' && n < sizeof(tag) - 1) {
        tag[n] = tagArg[n];
        ++n;
    }
    tag[n] = '\0';
    if (n == 0) {
        strcpy(tag, "manual");
    }
    if (!lock()) {
        Serial.printf("%s busy\n", kTag);
        return;
    }
    const PointInfo pi = {true, -1, tag, false, false, 0};
    takePoint(pi);
    unlock();
}

void cmdDiff() {
    if (!lock()) {
        Serial.printf("%s busy\n", kTag);
        return;
    }
    if (!s_prevValid || !s_olderValid) {
        Serial.printf("%s diff: need two stored points (take a point or `usb.hm snap`)\n", kTag);
    } else {
        scanTasks();
        printDiff(*s_prev, *s_cur, nullptr, kPersistentLines, false);
    }
    unlock();
}

void cmdDump(bool full) {
    if (!lock()) {
        Serial.printf("%s busy\n", kTag);
        return;
    }
    if (!ensureArena()) {
        unlock();
        return;
    }
    walkInto(*s_cur, -1, "dump");  // не міняє s_prev; s_cur перезаписано -> `usb.hm diff` потребує нової точки
    s_olderValid = false;
    scanTasks();
    const Slot& s = *s_cur;
    emit("%s dump%s t=%u INT free=%u largest=%u used=%ublk/%uB free_blk=%u recs=%u%s walk=%uus", kTag, full ? " full" : "",
         static_cast<unsigned>(s.tMs), static_cast<unsigned>(s.freeBytes), static_cast<unsigned>(s.largest),
         static_cast<unsigned>(s.usedBlk), static_cast<unsigned>(s.usedBytes), static_cast<unsigned>(s.freeBlk),
         static_cast<unsigned>(s.n), s.trunc ? " TRUNCATED" : "", static_cast<unsigned>(s.walkUs));
    const uint32_t usedMin = full ? kSmallBlockBytes : kDumpUsedMinBytes;
    uint32_t lines = 0;
    uint32_t skipped = 0;
    uint32_t smallN = 0;
    uint32_t smallB = 0;
    for (uint32_t i = 0; i < s.n; ++i) {
        const bool used = (s.recs[i].szUsed & kUsedBit) != 0;
        const uint32_t sz = s.recs[i].szUsed & ~kUsedBit;
        if (used && sz < usedMin) {
            ++smallN;
            smallB += sz;
            continue;
        }
        if (!full && lines >= kDumpMaxLines) {
            ++skipped;
            continue;
        }
        if (used) {
            char lb[72];
            label(s.recs[i].addr, sz, lb, sizeof(lb));
            emit("%s   used 0x%08x size=%u %s", kTag, static_cast<unsigned>(s.recs[i].addr), static_cast<unsigned>(sz), lb);
        } else {
            emit("%s   free 0x%08x size=%u%s", kTag, static_cast<unsigned>(s.recs[i].addr), static_cast<unsigned>(sz),
                 s.recs[i].addr == s.largestAddr ? "  <-- largest" : "");
        }
        if ((++lines % kDumpYieldEvery) == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));  // не переповнювати буфер Serial
        }
    }
    emit("%s dump end: %u lines, used < %u B aggregated: %u blk/%u B%s", kTag, static_cast<unsigned>(lines),
         static_cast<unsigned>(usedMin), static_cast<unsigned>(smallN), static_cast<unsigned>(smallB),
         skipped != 0 ? " (line limit hit: use `usb.hm dump full`)" : "");
    unlock();
}

// Перший файл із розширенням з kAudioExt у корені тому. Повертає шлях відносно тому («/name.mp3»).
bool findFirstTrack(char* out, size_t cap) {
    if (!usb_msc_mount::volumeLock(1000)) {
        return false;
    }
    bool found = false;
    DIR* d = opendir(usb_msc_mount::mountPath());
    if (d != nullptr) {
        while (!found) {
            const dirent* e = readdir(d);
            if (e == nullptr) {
                break;
            }
            const char* nm = e->d_name;
            const size_t len = strlen(nm);
            if (nm[0] == '.' || len + 2 > cap) {
                continue;
            }
            for (size_t k = 0; k < kAudioExtCount && !found; ++k) {
                const size_t el = strlen(kAudioExt[k]);
                if (len > el && strcasecmp(nm + len - el, kAudioExt[k]) == 0) {
                    snprintf(out, cap, "/%s", nm);
                    found = true;
                }
            }
        }
        closedir(d);
    }
    usb_msc_mount::volumeUnlock();
    return found;
}

bool waitFor(bool (*pred)(), uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (!pred()) {
        if (millis() - t0 >= timeoutMs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kCyclePollMs));
    }
    return true;
}

bool isReady() {
    const usb_mode::State s = usb_mode::state();
    return s == usb_mode::State::Ready || s == usb_mode::State::Playing || s == usb_mode::State::Error;
}
bool isOff() { return usb_mode::state() == usb_mode::State::Off || usb_mode::state() == usb_mode::State::Error; }
bool isFilePlaying() { return AudioPlayer::fileActive() && AudioPlayer::state() == PlayerState::Playing; }

// usb.hm cycle <N> [<сек файла>]: «вхід -> (перший трек -> пауза <сек>) -> вихід -> чекати результат радіо».
// Без <сек> трек не відтворюється (БАЗА). Зупиняється на першому збої підключення радіо, щоб стан не стерся.
void cmdCycle(const char* arg) {
    const long n = atol(arg);
    const char* p = arg;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        ++p;
    }
    p = skipWs(p);
    const long sec = (*p != '\0') ? atol(p) : 0;
    if (n < 1 || n > static_cast<long>(kCycleMaxCount) || sec < 0 || sec > static_cast<long>(kCycleFileMaxSec)) {
        Serial.printf("%s usage: usb.hm cycle <N 1..%u> [<file sec 1..%u>]  (no seconds = no file, BASE cycles)\n", kTag,
                      static_cast<unsigned>(kCycleMaxCount), static_cast<unsigned>(kCycleFileMaxSec));
        return;
    }
    if (usb_mode::state() != usb_mode::State::Off) {
        Serial.printf("%s cycle refused: usb_mode state=%s (needs Off)\n", kTag, usb_mode::stateName(usb_mode::state()));
        return;
    }
    if (!s_autoOn.load()) {
        s_autoOn.store(true);
        Serial.printf("%s cycle: auto points switched ON\n", kTag);
    }
    Serial.printf("%s cycle: N=%ld file=%lds (loopTask is blocked until the cycles end; radio must be the current input)\n",
                  kTag, n, sec);
    for (long c = 1; c <= n; ++c) {
        const uint32_t seq0 = s_resultSeq.load();
        const char* why = nullptr;
        if (!usb_mode::requestEnter(&why)) {
            Serial.printf("%s cycle %ld/%ld: enter refused (%s) - stop\n", kTag, c, n, why != nullptr ? why : "?");
            return;
        }
        if (!waitFor(isReady, usb_mode_cfg::kCycleEnterWaitMs) || usb_mode::state() == usb_mode::State::Error) {
            Serial.printf("%s cycle %ld/%ld: enter failed (state=%s) - stop\n", kTag, c, n, usb_mode::stateName(usb_mode::state()));
            return;
        }
        bool filePlayed = false;
        if (sec > 0) {
            char path[file_player_cfg::kPathMax];
            if (usb_mode::media() != usb_mode::Media::Mounted) {
                Serial.printf("%s cycle %ld/%ld: media=%s - cycle without a file\n", kTag, c, n, usb_mode::mediaName(usb_mode::media()));
            } else if (!findFirstTrack(path, sizeof(path))) {
                Serial.printf("%s cycle %ld/%ld: no audio file in the volume root - cycle without a file\n", kTag, c, n);
            } else if (!AudioPlayer::playFile(path, -1)) {
                Serial.printf("%s cycle %ld/%ld: playFile('%s') refused - cycle without a file\n", kTag, c, n, path);
            } else if (!waitFor(isFilePlaying, kCycleFileStartWaitMs)) {
                Serial.printf("%s cycle %ld/%ld: file did not start in %u ms\n", kTag, c, n, static_cast<unsigned>(kCycleFileStartWaitMs));
            } else {
                filePlayed = true;
                const uint32_t t0 = millis();
                while (millis() - t0 < static_cast<uint32_t>(sec) * 1000u && AudioPlayer::fileActive()) {
                    vTaskDelay(pdMS_TO_TICKS(kCyclePollMs));
                }
            }
        }
        usb_mode::requestExit(usb_mode::ExitCause::User);
        if (!waitFor(isOff, usb_mode_cfg::kCycleExitWaitMs) || usb_mode::state() == usb_mode::State::Error) {
            Serial.printf("%s cycle %ld/%ld: exit failed (state=%s) - stop\n", kTag, c, n, usb_mode::stateName(usb_mode::state()));
            return;
        }
        const uint32_t w0 = millis();
        while (s_resultSeq.load() == seq0 && millis() - w0 < kCycleResultWaitMs) {
            vTaskDelay(pdMS_TO_TICKS(kCyclePollMs));
        }
        const bool got = s_resultSeq.load() != seq0;
        const int ok = s_lastResultOk.load();
        Serial.printf("%s cycle %ld/%ld done: file=%d radio_connect=%s (%u ms)\n", kTag, c, n, filePlayed ? 1 : 0,
                      !got ? "NO RESULT (timeout; is the radio the current input?)" : (ok ? "OK" : "FAIL"),
                      static_cast<unsigned>(s_lastResultMs.load()));
        if (got && ok == 0) {
            Serial.printf("%s cycle stopped at the first failure: run `usb.hm dump full` and `usb.heap` NOW, then reboot\n", kTag);
            return;
        }
        // Дати радіо зазвучати, щоб наступний C0 відбувся при працюючому радіо.
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    Serial.printf("%s cycle finished: %ld cycles without failure\n", kTag, n);
}

}  // namespace

void handleConsoleLine(const char* line) {
    const char* rest = nullptr;
    if (!word(line, "usb.hm", &rest)) {
        Serial.printf("%s unknown command\n", kTag);
        return;
    }
    const char* arg = nullptr;
    if (word(rest, "stat", &arg)) {
        cmdStat();
    } else if (word(rest, "snap", &arg)) {
        cmdSnap(arg);
    } else if (word(rest, "diff", &arg)) {
        cmdDiff();
    } else if (word(rest, "dump", &arg)) {
        cmdDump(strncmp(arg, "full", 4) == 0);
    } else if (word(rest, "auto", &arg)) {
        if (strncmp(arg, "on", 2) == 0) {
            s_autoOn.store(true);
            s_skipC0 = false;
        } else if (strncmp(arg, "off", 3) == 0) {
            s_autoOn.store(false);
        }
        Serial.printf("%s auto=%s\n", kTag, s_autoOn.load() ? "on" : "off");
    } else if (word(rest, "cycle", &arg)) {
        cmdCycle(arg);
    } else {
        Serial.printf("%s usage: usb.hm stat | snap <tag> | diff | dump [full] | auto on|off | cycle <N> [<file sec>]\n", kTag);
    }
}

}  // namespace usb_heapmap

#endif  // USB_HEAPMAP
