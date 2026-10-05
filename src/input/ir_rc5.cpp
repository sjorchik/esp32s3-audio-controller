#include "input/ir_rc5.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config/ir_config.h"
#include "config/pins.h"
#include "core/events.h"

#if IR_DEBUG
#define IR_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define IR_LOG(...) ((void)0)
#endif

namespace {

// ---------------------------------------------------------------------------
// Константи протоколу (не налаштовуються)
// ---------------------------------------------------------------------------
constexpr uint8_t kFrameBits     = 14;              // S1 S2 T A4..A0 C5..C0
constexpr uint8_t kFrameHalfBits = kFrameBits * 2;  // 28
constexpr uint8_t kRc5xCmdBit    = 0x40;            // 7-й біт команди

// Усі дії до останньої (GAIN_DOWN, Prompt 7) включно: BASS_UP..GAIN_DOWN ідуть у enum Action одразу після DIGIT_9.
constexpr uint8_t kActionCount = static_cast<uint8_t>(Action::GAIN_DOWN) + 1;

constexpr size_t kEntryBytes   = 3;  // action, addr, cmd
constexpr size_t kBlobMaxBytes = 2 + ir_cfg::kMaxMapEntries * kEntryBytes + 4;

// ---------------------------------------------------------------------------
// Імена дій для JSON
// ---------------------------------------------------------------------------
struct ActionName {
    Action action;
    const char* name;
};

const ActionName kActionNames[] = {
    {Action::POWER, "POWER"},
    {Action::UP, "UP"},
    {Action::DOWN, "DOWN"},
    {Action::LEFT, "LEFT"},
    {Action::RIGHT, "RIGHT"},
    {Action::OK, "OK"},
    {Action::ENC_CW, "ENC_CW"},
    {Action::ENC_CCW, "ENC_CCW"},
    {Action::ENC_PRESS, "ENC_PRESS"},
    {Action::VOL_UP, "VOL_UP"},
    {Action::VOL_DOWN, "VOL_DOWN"},
    {Action::MUTE, "MUTE"},
    {Action::MENU, "MENU"},
    {Action::BACK, "BACK"},
    {Action::INPUT_RADIO, "INPUT_RADIO"},
    {Action::INPUT_TV, "INPUT_TV"},
    {Action::INPUT_PC, "INPUT_PC"},
    {Action::INPUT_AUX, "INPUT_AUX"},
    {Action::DIGIT_0, "DIGIT_0"},
    {Action::DIGIT_1, "DIGIT_1"},
    {Action::DIGIT_2, "DIGIT_2"},
    {Action::DIGIT_3, "DIGIT_3"},
    {Action::DIGIT_4, "DIGIT_4"},
    {Action::DIGIT_5, "DIGIT_5"},
    {Action::DIGIT_6, "DIGIT_6"},
    {Action::DIGIT_7, "DIGIT_7"},
    {Action::DIGIT_8, "DIGIT_8"},
    {Action::DIGIT_9, "DIGIT_9"},
    {Action::BASS_UP, "BASS_UP"},
    {Action::BASS_DOWN, "BASS_DOWN"},
    {Action::TREBLE_UP, "TREBLE_UP"},
    {Action::TREBLE_DOWN, "TREBLE_DOWN"},
    {Action::BALANCE_UP, "BALANCE_UP"},
    {Action::BALANCE_DOWN, "BALANCE_DOWN"},
    {Action::GAIN_UP, "GAIN_UP"},
    {Action::GAIN_DOWN, "GAIN_DOWN"},
};
static_assert(sizeof(kActionNames) / sizeof(kActionNames[0]) == kActionCount,
              "kActionNames must list every Action");

const char* nameOf(uint8_t action) {
    for (const ActionName& n : kActionNames) {
        if (static_cast<uint8_t>(n.action) == action) return n.name;
    }
    return "?";
}

bool actionFromName(const char* name, uint8_t& out) {
    if (name == nullptr) return false;
    for (const ActionName& n : kActionNames) {
        if (strcmp(n.name, name) == 0) {
            out = static_cast<uint8_t>(n.action);
            return true;
        }
    }
    return false;
}

// Дії, для яких має сенс утримання (repeat=true).
bool isRepeatable(Action a) {
    switch (a) {
        case Action::VOL_UP:
        case Action::VOL_DOWN:
        case Action::UP:
        case Action::DOWN:
        case Action::LEFT:
        case Action::RIGHT:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Стан модуля
// ---------------------------------------------------------------------------
struct MapEntry {
    uint8_t action;
    uint8_t addr;
    uint8_t cmd;
};

SemaphoreHandle_t gMutex = nullptr;  // захищає gMap* та gLearn*
QueueHandle_t gRxQueue = nullptr;
rmt_channel_handle_t gRxChan = nullptr;
rmt_receive_config_t gRxCfg = {};
rmt_symbol_word_t gRxBuf[ir_cfg::kRmtRxBufSymbols];
TaskHandle_t gTask = nullptr;
bool gStarted = false;

volatile uint32_t gDropped = 0;  // пише лише задача приймання
volatile uint32_t gErrors = 0;

MapEntry gMap[ir_cfg::kMaxMapEntries];
uint8_t gMapCount = 0;

IrRc5::LearnStatus gLearnStatus = IrRc5::LearnStatus::Idle;
Action gLearnAction = Action::POWER;
bool gHaveCand = false;
uint8_t gCandAddr = 0;
uint8_t gCandCmd = 0;
bool gHaveConflict = false;
Action gConflictAction = Action::POWER;
uint32_t gLearnDeadline = 0;

struct Lock {
    Lock() { xSemaphoreTake(gMutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(gMutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

bool expired(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

// ---------------------------------------------------------------------------
// Мапа (усі функції *Locked викликати під мʼютексом)
// ---------------------------------------------------------------------------
int findCode(uint8_t addr, uint8_t cmd) {
    for (uint8_t i = 0; i < gMapCount; ++i) {
        if (gMap[i].addr == addr && gMap[i].cmd == cmd) return i;
    }
    return -1;
}

int findAction(uint8_t action) {
    for (uint8_t i = 0; i < gMapCount; ++i) {
        if (gMap[i].action == action) return i;
    }
    return -1;
}

void removeIndex(int idx) {
    if (idx < 0 || idx >= gMapCount) return;
    memmove(&gMap[idx], &gMap[idx + 1], (gMapCount - idx - 1) * sizeof(MapEntry));
    --gMapCount;
}

size_t removeAllOfAction(uint8_t action) {
    size_t removed = 0;
    int idx;
    while ((idx = findAction(action)) >= 0) {
        removeIndex(idx);
        ++removed;
    }
    return removed;
}

bool addEntry(uint8_t action, uint8_t addr, uint8_t cmd) {
    if (gMapCount >= ir_cfg::kMaxMapEntries) return false;
    gMap[gMapCount++] = {action, addr, cmd};
    return true;
}

// ---------------------------------------------------------------------------
// NVS: blob = [version][count][count × (action, addr, cmd)][crc32 LE]
// ---------------------------------------------------------------------------
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool saveLocked() {
    uint8_t buf[kBlobMaxBytes];
    size_t n = 0;
    buf[n++] = ir_cfg::kMapFormatVersion;
    buf[n++] = gMapCount;
    for (uint8_t i = 0; i < gMapCount; ++i) {
        buf[n++] = gMap[i].action;
        buf[n++] = gMap[i].addr;
        buf[n++] = gMap[i].cmd;
    }
    const uint32_t crc = crc32(buf, n);
    for (int i = 0; i < 4; ++i) buf[n++] = static_cast<uint8_t>((crc >> (8 * i)) & 0xFF);

    Preferences prefs;
    if (!prefs.begin(ir_cfg::kNvsNamespace, false)) {
        Serial.println("[IR] WARN: NVS open failed, map not saved");
        return false;
    }
    const size_t written = prefs.putBytes(ir_cfg::kNvsMapKey, buf, n);
    prefs.end();
    if (written != n) {
        Serial.println("[IR] WARN: NVS write failed, map not saved");
        return false;
    }
    IR_LOG("[IR] map saved, entries=%u\n", static_cast<unsigned>(gMapCount));
    return true;
}

// Розбирає blob у out/count. Повертає nullptr при успіху, інакше причину.
const char* parseBlob(const uint8_t* buf, size_t len, MapEntry* out, uint8_t& count) {
    if (len < 6) return "blob too short";
    if (buf[0] != ir_cfg::kMapFormatVersion) return "format version mismatch";
    const uint8_t n = buf[1];
    if (n > ir_cfg::kMaxMapEntries) return "too many entries";
    if (len != 2 + static_cast<size_t>(n) * kEntryBytes + 4) return "bad length";

    const size_t body = len - 4;
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored |= static_cast<uint32_t>(buf[body + i]) << (8 * i);
    if (stored != crc32(buf, body)) return "CRC mismatch";

    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t* e = &buf[2 + i * kEntryBytes];
        if (e[0] >= kActionCount || e[1] > 31 || e[2] > 127) return "invalid entry";
        for (uint8_t j = 0; j < i; ++j) {
            if (out[j].addr == e[1] && out[j].cmd == e[2]) return "duplicate code";
        }
        out[i] = {e[0], e[1], e[2]};
    }
    count = n;
    return nullptr;
}

bool loadLocked() {
    uint8_t buf[kBlobMaxBytes];
    size_t len = 0;
    {
        Preferences prefs;
        if (!prefs.begin(ir_cfg::kNvsNamespace, false)) {
            Serial.println("[IR] WARN: NVS open failed, map empty");
            gMapCount = 0;
            return false;
        }
        len = prefs.getBytesLength(ir_cfg::kNvsMapKey);
        if (len == 0) {
            prefs.end();
            gMapCount = 0;
            IR_LOG("[IR] map: nothing stored\n");
            return false;
        }
        if (len > sizeof(buf) || prefs.getBytes(ir_cfg::kNvsMapKey, buf, len) != len) {
            prefs.end();
            gMapCount = 0;
            Serial.println("[IR] WARN: map read failed, map empty");
            return false;
        }
        prefs.end();
    }

    MapEntry tmp[ir_cfg::kMaxMapEntries];
    uint8_t count = 0;
    const char* err = parseBlob(buf, len, tmp, count);
    if (err != nullptr) {
        Serial.printf("[IR] WARN: stored map rejected (%s), map empty\n", err);
        gMapCount = 0;
        return false;
    }
    memcpy(gMap, tmp, count * sizeof(MapEntry));
    gMapCount = count;
    IR_LOG("[IR] map loaded, entries=%u\n", static_cast<unsigned>(gMapCount));
    return true;
}

// ---------------------------------------------------------------------------
// Декодер RC5/RC5X
// ---------------------------------------------------------------------------

// Скільки півбітів (1 або 2) вміщує тривалість; 0 — поза допуском.
uint8_t halfBitCount(uint32_t durUs) {
    const uint32_t t = ir_cfg::kHalfBitUs;
    const uint32_t tol = ir_cfg::kHalfBitTolerancePercent;
    for (uint32_t n = 1; n <= 2; ++n) {
        const uint32_t nominal = n * t;
        const uint32_t lo = nominal * (100 - tol) / 100;
        const uint32_t hi = nominal * (100 + tol) / 100;
        if (durUs >= lo && durUs <= hi) return static_cast<uint8_t>(n);
    }
    return 0;
}

// Розбір символів RMT у кадр. Вихід VS1838B інвертований: рівень kMarkLevel
// (LOW) = несуча («mark»), інший = пауза («space»).
//
// RC5 Manchester: біт 1 = space, потім mark; біт 0 = mark, потім space.
// Лінія в спокої = space, тому перший півбіт S1 (space) у записі відсутній:
// перший захоплений сегмент — це mark другої половини S1. Додаємо його віртуально.
// Останній space (кінець кадру) зливається зі спокоєм, тому може бути
// відсутній у записі — теж додаємо за потреби.
bool decodeFrame(const rmt_symbol_word_t* sym, size_t numSym, IrRc5Frame& out) {
    struct Seg {
        uint8_t level;
        uint32_t dur;
    };
    Seg segs[ir_cfg::kMaxSegments];
    size_t ns = 0;

    // 1. Розгортаємо символи в послідовність сегментів, зливаючи сусідні однакові рівні.
    bool ended = false;
    for (size_t i = 0; i < numSym && !ended; ++i) {
        for (int h = 0; h < 2; ++h) {
            const uint8_t level = h ? sym[i].level1 : sym[i].level0;
            const uint32_t dur = h ? sym[i].duration1 : sym[i].duration0;
            if (dur == 0) {  // маркер кінця
                ended = true;
                break;
            }
            if (ns > 0 && segs[ns - 1].level == level) {
                segs[ns - 1].dur += dur;
            } else {
                if (ns >= ir_cfg::kMaxSegments) return false;
                segs[ns].level = level;
                segs[ns].dur = dur;
                ++ns;
            }
        }
    }
    if (ns == 0) return false;

    // 2. Сегменти -> півбіти (bit k у halves: 1 = mark).
    uint32_t halves = 0;
    uint8_t cnt = 0;
    bool lastMark = false;
    auto push = [&](bool mark, uint8_t n) -> bool {
        for (uint8_t i = 0; i < n; ++i) {
            if (cnt >= kFrameHalfBits) return false;
            if (mark) halves |= (1u << cnt);
            ++cnt;
        }
        lastMark = mark;
        return true;
    };

    push(false, 1);  // віртуальний space: перша половина S1

    for (size_t k = 0; k < ns; ++k) {
        const bool mark = (segs[k].level == ir_cfg::kMarkLevel);
        if (k == 0 && !mark) return false;  // кадр має починатися з несучої

        if (k == ns - 1 && !mark) {  // завершальний space: рівно один півбіт
            if (!push(false, 1)) return false;
            continue;
        }

        int32_t d = static_cast<int32_t>(segs[k].dur) + (mark ? ir_cfg::kMarkBiasUs : -ir_cfg::kMarkBiasUs);
        if (d < 0) d = 0;
        const uint8_t n = halfBitCount(static_cast<uint32_t>(d));
        if (n == 0) return false;
        if (!push(mark, n)) return false;
    }

    // Завершальний space відсутній у записі (кадр закінчився бітом 0).
    if (cnt == kFrameHalfBits - 1 && lastMark) push(false, 1);
    if (cnt != kFrameHalfBits) return false;

    // 3. Manchester: пара півбітів (перший, другий) -> біт. Однакові = порушення.
    uint16_t bits = 0;  // MSB = S1
    for (uint8_t i = 0; i < kFrameBits; ++i) {
        const bool first = (halves >> (2 * i)) & 1u;
        const bool second = (halves >> (2 * i + 1)) & 1u;
        if (first == second) return false;
        bits = static_cast<uint16_t>((bits << 1) | (second ? 1u : 0u));  // space->mark = 1
    }

    const bool s1 = (bits >> 13) & 1u;
    const bool s2 = (bits >> 12) & 1u;
    const bool tog = (bits >> 11) & 1u;
    if (!s1) return false;  // S1 завжди 1

    out.addr = static_cast<uint8_t>((bits >> 6) & 0x1F);
    // RC5X: S2 — інверсія 7-го біта команди. S2=1 -> біт 0 (RC5, cmd 0..63),
    // S2=0 -> біт 1 (RC5X, cmd 64..127).
    out.cmd = static_cast<uint8_t>((bits & 0x3F) | (s2 ? 0 : kRc5xCmdBit));
    out.toggle = tog;
    out.isRC5X = !s2;
    out.timestampMs = 0;  // ставить викликач
    return true;
}

// ---------------------------------------------------------------------------
// Відстеження натискання / утримання (лише задача приймання)
// ---------------------------------------------------------------------------
struct Tracker {
    bool have;
    uint8_t addr;
    uint8_t cmd;
    bool toggle;
    uint32_t lastMs;
    uint32_t pressStartMs;
    bool consumed;  // натискання використане навчанням: решту його кадрів ігнорувати
};

// true = нове натискання, false = повтор (утримання).
bool classify(Tracker& t, const IrRc5Frame& f) {
    const bool repeat = t.have && t.addr == f.addr && t.cmd == f.cmd && t.toggle == f.toggle &&
                        (f.timestampMs - t.lastMs) <= ir_cfg::kRepeatGapMs;
    if (!repeat) {
        t.pressStartMs = f.timestampMs;
        t.consumed = false;
    }
    t.have = true;
    t.addr = f.addr;
    t.cmd = f.cmd;
    t.toggle = f.toggle;
    t.lastMs = f.timestampMs;
    return !repeat;
}

void postAction(Action a, bool repeat) {
    Event e;
    e.action = a;
    e.source = EventSource::IR;
    e.repeat = repeat;
    e.longPress = false;
    e.delta = 0;
    if (!EventBus::post(e, 0)) ++gDropped;
}

// ---------------------------------------------------------------------------
// Навчання (під мʼютексом)
// ---------------------------------------------------------------------------
bool learnActiveLocked() {
    return gLearnStatus == IrRc5::LearnStatus::Waiting || gLearnStatus == IrRc5::LearnStatus::Confirm ||
           gLearnStatus == IrRc5::LearnStatus::Conflict;
}

// Привʼязує кандидата до цілі: старі коди цілі та можливий конфліктний запис
// видаляються, новий додається, мапа зберігається.
void applyLearnLocked() {
    const uint8_t target = static_cast<uint8_t>(gLearnAction);
    const int clash = findCode(gCandAddr, gCandCmd);
    if (clash >= 0) removeIndex(clash);
    removeAllOfAction(target);
    if (!addEntry(target, gCandAddr, gCandCmd)) {  // недосяжно: beginLearn це перевіряє
        Serial.println("[IR] WARN: map full, learn aborted");
        gLearnStatus = IrRc5::LearnStatus::Idle;
        gHaveCand = false;
        gHaveConflict = false;
        return;
    }
    saveLocked();
    gLearnStatus = IrRc5::LearnStatus::Success;
    gHaveConflict = false;
    IR_LOG("[IR] learn: OK %s -> addr=%u cmd=%u\n", nameOf(target), static_cast<unsigned>(gCandAddr),
           static_cast<unsigned>(gCandCmd));
}

void learnFrameLocked(const IrRc5Frame& f, bool isNew) {
    if (!isNew) return;  // утримання не рахуємо як натискання

    switch (gLearnStatus) {
        case IrRc5::LearnStatus::Waiting:
            gCandAddr = f.addr;
            gCandCmd = f.cmd;
            gHaveCand = true;
            gLearnStatus = IrRc5::LearnStatus::Confirm;
            gLearnDeadline = f.timestampMs + ir_cfg::kLearnTimeoutMs;
            IR_LOG("[IR] learn: candidate addr=%u cmd=%u, press again to confirm\n",
                   static_cast<unsigned>(f.addr), static_cast<unsigned>(f.cmd));
            break;

        case IrRc5::LearnStatus::Confirm:
            if (f.addr == gCandAddr && f.cmd == gCandCmd) {
                const int clash = findCode(gCandAddr, gCandCmd);
                if (clash >= 0 && gMap[clash].action != static_cast<uint8_t>(gLearnAction)) {
                    gConflictAction = static_cast<Action>(gMap[clash].action);
                    gHaveConflict = true;
                    gLearnStatus = IrRc5::LearnStatus::Conflict;
                    gLearnDeadline = f.timestampMs + ir_cfg::kLearnConflictTimeoutMs;
                    IR_LOG("[IR] learn: CONFLICT addr=%u cmd=%u already bound to %s\n",
                           static_cast<unsigned>(gCandAddr), static_cast<unsigned>(gCandCmd),
                           nameOf(gMap[clash].action));
                } else {
                    applyLearnLocked();
                }
            } else {  // інший код: вважаємо новим кандидатом
                gCandAddr = f.addr;
                gCandCmd = f.cmd;
                gLearnDeadline = f.timestampMs + ir_cfg::kLearnTimeoutMs;
                IR_LOG("[IR] learn: code changed, new candidate addr=%u cmd=%u\n",
                       static_cast<unsigned>(f.addr), static_cast<unsigned>(f.cmd));
            }
            break;

        default:  // Conflict: кадри ігноруємо, чекаємо confirmOverwrite()/cancelLearn()
            break;
    }
}

void learnTick(uint32_t now) {
    Lock lock;
    if (learnActiveLocked() && expired(now, gLearnDeadline)) {
        gLearnStatus = IrRc5::LearnStatus::Timeout;
        gHaveCand = false;
        gHaveConflict = false;
        IR_LOG("[IR] learn: timeout\n");
    }
}

// ---------------------------------------------------------------------------
// Обробка валідного кадру
// ---------------------------------------------------------------------------
void handleFrame(const IrRc5Frame& f, Tracker& t) {
    IR_LOG("[IR] addr=%u cmd=%u toggle=%u rc5x=%u\n", static_cast<unsigned>(f.addr), static_cast<unsigned>(f.cmd),
           static_cast<unsigned>(f.toggle), static_cast<unsigned>(f.isRC5X));

    const bool isNew = classify(t, f);

    uint8_t actRaw = 0;
    bool known = false;
    {
        Lock lock;
        if (learnActiveLocked()) {  // під час навчання Action-події не генеруємо
            learnFrameLocked(f, isNew);
            t.consumed = true;
            return;
        }
        if (t.consumed) return;  // хвіст натискання, використаного навчанням
        const int idx = findCode(f.addr, f.cmd);
        if (idx >= 0) {
            actRaw = gMap[idx].action;
            known = true;
        }
    }
    if (!known) return;

    const Action act = static_cast<Action>(actRaw);
    if (isNew) {
        postAction(act, false);
    } else if (isRepeatable(act) && (f.timestampMs - t.pressStartMs) >= ir_cfg::kRepeatDelayMs) {
        postAction(act, true);
    }
}

// ---------------------------------------------------------------------------
// RMT
// ---------------------------------------------------------------------------
bool IRAM_ATTR onRxDone(rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata, void* ctx) {
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(static_cast<QueueHandle_t>(ctx), edata, &woken);
    return woken == pdTRUE;
}

bool armReceive() {
    return rmt_receive(gRxChan, gRxBuf, sizeof(gRxBuf), &gRxCfg) == ESP_OK;
}

void irTask(void*) {
    Tracker tracker = {};
    bool armed = armReceive();
    rmt_rx_done_event_data_t ev;

    for (;;) {
        if (!armed) armed = armReceive();

        if (xQueueReceive(gRxQueue, &ev, pdMS_TO_TICKS(ir_cfg::kTaskTickMs)) == pdTRUE) {
            IrRc5Frame frame;
            if (decodeFrame(ev.received_symbols, ev.num_symbols, frame)) {
                frame.timestampMs = millis();
                handleFrame(frame, tracker);
            } else {
                ++gErrors;
            }
            armed = armReceive();  // буфер вільний лише після обробки
        }
        learnTick(millis());
    }
}

}  // namespace

// ===========================================================================
// Публічний інтерфейс
// ===========================================================================

bool IrRc5::begin() {
    if (gStarted) return true;

    if (!EventBus::isReady()) {
        Serial.println("[IR] ERROR: EventBus not ready");
        return false;
    }

    if (gMutex == nullptr) gMutex = xSemaphoreCreateMutex();
    if (gRxQueue == nullptr) gRxQueue = xQueueCreate(ir_cfg::kRxQueueLen, sizeof(rmt_rx_done_event_data_t));
    if (gMutex == nullptr || gRxQueue == nullptr) {
        Serial.println("[IR] ERROR: no memory for mutex/queue");
        return false;
    }

    {
        Lock lock;
        loadLocked();  // порожня/пошкоджена мапа не є помилкою старту
    }

    rmt_rx_channel_config_t rxCfg = {};
    rxCfg.gpio_num = static_cast<gpio_num_t>(pins::kIrIn);
    rxCfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rxCfg.resolution_hz = ir_cfg::kRmtResolutionHz;
    rxCfg.mem_block_symbols = ir_cfg::kRmtMemBlockSymbols;

    if (rmt_new_rx_channel(&rxCfg, &gRxChan) != ESP_OK) {
        Serial.println("[IR] ERROR: rmt_new_rx_channel failed");
        gRxChan = nullptr;
        return false;
    }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = onRxDone;
    if (rmt_rx_register_event_callbacks(gRxChan, &cbs, gRxQueue) != ESP_OK || rmt_enable(gRxChan) != ESP_OK) {
        Serial.println("[IR] ERROR: RMT callbacks/enable failed");
        rmt_del_channel(gRxChan);
        gRxChan = nullptr;
        return false;
    }

    gRxCfg = {};
    gRxCfg.signal_range_min_ns = ir_cfg::kRmtMinSignalNs;
    gRxCfg.signal_range_max_ns = ir_cfg::kRmtIdleThresholdUs * 1000u;

    if (xTaskCreatePinnedToCore(irTask, "ir_rc5", ir_cfg::kTaskStackBytes, nullptr, ir_cfg::kTaskPriority, &gTask,
                                ir_cfg::kTaskCore) != pdPASS) {
        Serial.println("[IR] ERROR: task create failed");
        rmt_disable(gRxChan);
        rmt_del_channel(gRxChan);
        gRxChan = nullptr;
        return false;
    }

    gStarted = true;
    IR_LOG("[IR] started on GPIO %d\n", pins::kIrIn);
    return true;
}

uint32_t IrRc5::droppedEvents() {
    return gDropped;
}

uint32_t IrRc5::decodeErrors() {
    return gErrors;
}

// --- Мапа ---

bool IrRc5::load() {
    if (gMutex == nullptr) return false;
    Lock lock;
    return loadLocked();
}

bool IrRc5::save() {
    if (gMutex == nullptr) return false;
    Lock lock;
    return saveLocked();
}

void IrRc5::clearMap() {
    if (gMutex == nullptr) return;
    Lock lock;
    gMapCount = 0;
    saveLocked();
}

bool IrRc5::removeAction(Action action) {
    if (gMutex == nullptr) return false;
    Lock lock;
    if (removeAllOfAction(static_cast<uint8_t>(action)) == 0) return false;
    saveLocked();
    return true;
}

bool IrRc5::lookup(uint8_t addr, uint8_t cmd, Action& out) {
    if (gMutex == nullptr) return false;
    Lock lock;
    const int idx = findCode(addr, cmd);
    if (idx < 0) return false;
    out = static_cast<Action>(gMap[idx].action);
    return true;
}

bool IrRc5::codeFor(Action action, uint8_t& addr, uint8_t& cmd) {
    if (gMutex == nullptr) return false;
    Lock lock;
    const int idx = findAction(static_cast<uint8_t>(action));
    if (idx < 0) return false;
    addr = gMap[idx].addr;
    cmd = gMap[idx].cmd;
    return true;
}

size_t IrRc5::mapSize() {
    if (gMutex == nullptr) return 0;
    Lock lock;
    return gMapCount;
}

// --- Навчання ---

bool IrRc5::beginLearn(Action action) {
    if (gMutex == nullptr) return false;
    const uint8_t a = static_cast<uint8_t>(action);
    if (a >= kActionCount) {
        Serial.printf("[IR] WARN: action id %u is not in the IR table\n", static_cast<unsigned>(a));
        return false;
    }

    Lock lock;
    if (gMapCount >= ir_cfg::kMaxMapEntries && findAction(a) < 0) {
        Serial.println("[IR] WARN: map full, cannot learn");
        return false;
    }
    gLearnAction = action;
    gHaveCand = false;
    gHaveConflict = false;
    gLearnStatus = LearnStatus::Waiting;
    gLearnDeadline = millis() + ir_cfg::kLearnTimeoutMs;
    IR_LOG("[IR] learn: start %s, press the remote button\n", nameOf(a));
    return true;
}

void IrRc5::cancelLearn() {
    if (gMutex == nullptr) return;
    Lock lock;
    if (gLearnStatus != LearnStatus::Idle) IR_LOG("[IR] learn: cancelled\n");
    gLearnStatus = LearnStatus::Idle;
    gHaveCand = false;
    gHaveConflict = false;
}

IrRc5::LearnStatus IrRc5::status() {
    if (gMutex == nullptr) return LearnStatus::Idle;
    Lock lock;
    return gLearnStatus;
}

bool IrRc5::confirmOverwrite() {
    if (gMutex == nullptr) return false;
    Lock lock;
    if (gLearnStatus != LearnStatus::Conflict) return false;
    IR_LOG("[IR] learn: overwrite confirmed\n");
    applyLearnLocked();
    return gLearnStatus == LearnStatus::Success;
}

Action IrRc5::learnTarget() {
    if (gMutex == nullptr) return Action::POWER;
    Lock lock;
    return gLearnAction;
}

bool IrRc5::learnCandidate(uint8_t& addr, uint8_t& cmd) {
    if (gMutex == nullptr) return false;
    Lock lock;
    if (!gHaveCand) return false;
    addr = gCandAddr;
    cmd = gCandCmd;
    return true;
}

bool IrRc5::learnConflictWith(Action& other) {
    if (gMutex == nullptr) return false;
    Lock lock;
    if (gLearnStatus != LearnStatus::Conflict || !gHaveConflict) return false;
    other = gConflictAction;
    return true;
}

// --- JSON ---

bool IrRc5::exportJson(JsonDocument& doc) {
    if (gMutex == nullptr) return false;
    JsonArray arr = doc.to<JsonArray>();
    Lock lock;
    for (uint8_t i = 0; i < gMapCount; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["action"] = nameOf(gMap[i].action);
        o["addr"] = gMap[i].addr;
        o["cmd"] = gMap[i].cmd;
        o["rc5x"] = gMap[i].cmd >= kRc5xCmdBit;
    }
    return !doc.overflowed();
}

bool IrRc5::importJson(const JsonDocument& doc) {
    if (gMutex == nullptr) return false;

    JsonArrayConst arr = doc.as<JsonArrayConst>();
    if (arr.isNull() || arr.size() > ir_cfg::kMaxMapEntries) return false;

    // Спершу повна валідація у тимчасовий масив.
    MapEntry tmp[ir_cfg::kMaxMapEntries];
    uint8_t count = 0;
    for (JsonVariantConst item : arr) {
        JsonObjectConst o = item.as<JsonObjectConst>();
        if (o.isNull()) return false;

        uint8_t action = 0;
        if (!actionFromName(o["action"].as<const char*>(), action)) return false;
        if (!o["addr"].is<int>() || !o["cmd"].is<int>()) return false;
        const int addr = o["addr"].as<int>();
        const int cmd = o["cmd"].as<int>();
        if (addr < 0 || addr > 31 || cmd < 0 || cmd > 127) return false;

        if (!o["rc5x"].isNull()) {  // необовʼязкове поле, але має бути узгоджене з cmd
            if (!o["rc5x"].is<bool>()) return false;
            if (o["rc5x"].as<bool>() != (cmd >= kRc5xCmdBit)) return false;
        }
        for (uint8_t j = 0; j < count; ++j) {
            if (tmp[j].addr == addr && tmp[j].cmd == cmd) return false;  // дубль коду
        }
        tmp[count++] = {action, static_cast<uint8_t>(addr), static_cast<uint8_t>(cmd)};
    }

    Lock lock;
    if (learnActiveLocked()) return false;
    memcpy(gMap, tmp, count * sizeof(MapEntry));
    gMapCount = count;
    return saveLocked();
}
