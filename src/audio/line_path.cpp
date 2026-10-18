// Прохід зовнішнього звуку PCM1808 -> ESP32-S3 -> PCM5102 (Prompt 44, [P44b] — життєвий цикл ресурсів).
// Опис — audio/line_path.h.
//
// Топологія (V2 з P43):
//   PCM1808 --DIN GPIO8--> I2S1 RX (slave) --[блок 128 кадрів, послаблення]--> I2S1 TX (slave)
//   --DOUT--> GPIO18 --> PCM5102. BCLK GPIO15 / WS GPIO17 веде I2S0 бібліотеки ESP32-audioI2S
//   (master, MCLK GPIO16 = 256·fs для PCM1808); I2S1 їх лише слухає, тож RX і TX синхронні
//   й звідти ж береться швидкість (ніякого ресемплінгу чи дрейфу між АЦП і ЦАП).
//
// GPIO18 фізично одна: у радіо-режимі її веде I2S0 (ESP32-audioI2S, setPinout), у режимі проходу —
// I2S1 TX. Перемикання — лише GPIO-матрицею (esp_rom_gpio_connect_out_signal), без зміни
// бібліотеки. Канал TX ініціалізується з dout = UNUSED, щоб драйвер IDF не перехопив GPIO18.
//
// ВАЖЛИВО про піни 15/17: вони водночас вихід I2S0 і вхід I2S1 (slave). Драйвер IDF при
// ініціалізації slave-входів може лишити пін «лише входом» — після init/enable повертаємо
// output-enable низькорівнево (gpio_ll), як це робить audio/adc_vu (P37). gpio_set_direction()
// не використовуємо: він може відключити вихідний сигнал піна від I2S0.
//
// [P44b] ЖИТТЄВИЙ ЦИКЛ. begin() нічого не виділяє (крім Serial-тесту). start() створює:
//   (1) буфер блоку (внутрішня RAM, 1 КБ), (2) задачу line_path, яка першою дією створює канали I2S1
//   (≈12 КБ DMA) і сигналить семафором результат. Якщо щось не вдалось — start() повертає false,
//   allocFailures++, XSMT low, GPIO18 на I2S0. returnToRadio() знищує канали й буфер і завершує задачу
//   (стек і TCB звільнить IDLE-задача трохи пізніше). stop() ресурси лишає.
//   Чому канали створює ЗАДАЧА, а не виклик start(): стек викликача (AppController / веб / тест) невідомий,
//   а стек задачі виміряно (≈3,2 КБ вільного мінімуму в P44). start() блокується на семафорі.
//
// Безпека: старт завжди під XSMT low, підсилення 0; мʼют знімається лише після kSettleBlocks
// стабільних блоків; при відсутності блоків понад kStallMs — XSMT low і підсилення 0 (TX має
// auto_clear, тож без даних у лінію йдуть нулі, а не «повна гучність»).
//
// [P44b] ЧИСТІ ПЕРЕХОДИ (гіпотеза за кодом, НЕ перевірено на залізі — перевіряти lp.cycle):
//   - TX увімкнений, а писати нічого (settle-блоки) -> тепер у TX пишуться нулі;
//   - 20 мс затримки після XSMT low при увімкнених каналах (> глибини черги DMA ≈16 мс) -> тепер це
//     вікно, у якому RX вичитується, а в TX пишуться нулі, і канали вимикаються одразу після;
//   - Serial.printf у критичному місці Starting -> Running (≈5 мс на рядок за 115200) -> лог перенесено
//     ПІСЛЯ запису першого блоку й обʼєднано в один рядок.
//
// Задача line_path — єдиний «власник» I2S1 і XSMT на час проходу. Прототип: XSMT керується тут
// напряму (digitalWrite), у P47 це перейде до dac_output.

#include "audio/line_path.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_player.h"
#include "config/line_path_config.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if LINE_PATH_DEBUG
#define LINE_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LINE_LOG(...) \
    do {              \
    } while (0)
#endif

namespace {

namespace cfg = line_path_cfg;

[[maybe_unused]] size_t heapFree() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

[[maybe_unused]] size_t heapLargest() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

// Діагностика ресурсів для Serial-тесту (заповнює реалізація нижче; при LINE_PATH_ENABLE == 0 — нулі).
struct DbgInfo {
    bool     alive;               // задача line_path існує (ресурси створені)
    uint32_t stackFreeMin;        // вільний мінімум стека задачі, Б (після видалення — останнє виміряне)
    uint32_t heapFreeBefore;      // внутрішня RAM перед створенням ресурсів (останній start із порожнього стану)
    uint32_t heapLargestBefore;
    uint32_t heapFreeAfter;       // те саме одразу після створення
    uint32_t heapLargestAfter;
    uint32_t recoveries;          // скільки разів вийшли зі Stalled (самовідновлення)
};

}  // namespace

#if LINE_PATH_ENABLE

#include "config/audio_player_config.h"
#include "driver/i2s_std.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"

static_assert(line_path_cfg::kXsmtMutedLevel == player_cfg::kXsmtMutedLevel &&
                  line_path_cfg::kXsmtUnmutedLevel == player_cfg::kXsmtUnmutedLevel,
              "XSMT levels must match player_cfg");

namespace {

enum Req : uint8_t { kReqNone = 0, kReqStart, kReqStop, kReqRadio };

constexpr int32_t kUnityQ16 = 65536;  // 1.0 у форматі Q16
constexpr size_t  kBufBytes = cfg::kBlockFrames * 2 * sizeof(int32_t);

// --- Стан (пише лише задача, крім s_req / s_attDb / лічильників ISR / s_taskAlive на старті) ---
volatile uint8_t       s_req = kReqNone;
volatile LinePathState s_state = LinePathState::Off;
volatile bool          s_muted = true;
volatile bool          s_routed = false;
volatile int8_t        s_attDb = cfg::kPrototypeAttenuationDb;
volatile uint32_t      s_rxOvf = 0;
volatile uint32_t      s_txOvf = 0;

// Життєвий цикл ресурсів.
bool              s_ready = false;
bool              s_opLock = false;          // серіалізує start / stop / returnToRadio між задачами
volatile bool     s_taskAlive = false;       // задача існує (разом із каналами й буфером)
volatile bool     s_initAbandoned = false;   // start() не дочекався створення каналів — задача сама згортається
volatile bool     s_exit = false;            // задача має завершитись (після returnToRadio / відкату)
volatile int      s_initErr = 0;             // esp_err_t створення каналів (0 = ESP_OK)
SemaphoreHandle_t s_initSem = nullptr;       // binary, статичний (без кучі)
StaticSemaphore_t s_initSemBuf;

bool              s_rxOn = false;
bool              s_txOn = false;
i2s_chan_handle_t s_rx = nullptr;
i2s_chan_handle_t s_tx = nullptr;
TaskHandle_t      s_task = nullptr;
int32_t*          s_buf = nullptr;   // інтерліврований L,R,L,R...; 24 біти вирівняні вліво в 32-бітному слові

// Підсилення: Q16, лінійний перехід по кадрах.
int32_t s_gCur = 0;
int32_t s_gTgt = 0;
int32_t s_gStep = 0;
int8_t  s_appliedAttDb = cfg::kPrototypeAttenuationDb;

uint32_t s_settle = 0;
uint32_t s_lastGoodMs = 0;
uint32_t s_startReqMs = 0;
bool     s_recovering = false;       // Starting після Stalled (не рахувати як start)
bool     s_pinsOk = true;            // результат restorePinOutputs() при останньому enable

// Ін'єкція збою (lp.fault): до цього моменту (millis) RX не споживається. 0 = нема.
volatile uint32_t s_faultUntilMs = 0;
uint32_t          s_faultRx0 = 0;
uint32_t          s_faultTx0 = 0;

// Статистика (читається з інших задач без блокувань; значення 32-бітні, вирівняні).
uint32_t s_blocks = 0, s_readTimeouts = 0, s_readErrors = 0, s_writeTimeouts = 0;
uint32_t s_stalls = 0, s_starts = 0, s_maxProcessUs = 0, s_maxReadWaitUs = 0;
uint32_t s_initMs = 0, s_lastStartMs = 0, s_allocFailures = 0, s_recoveries = 0;
uint32_t s_peakL = 0, s_peakR = 0;  // |x| у шкалі 2^31, макс. з останнього resetStats()

// Діагностика пам'яті / стека.
uint32_t s_heapFreeBefore = 0, s_heapLargestBefore = 0, s_heapFreeAfter = 0, s_heapLargestAfter = 0;
uint32_t s_stackFreeMin = 0;

// ---------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------

// quiet = true: лише рівень піна й прапорець, без логу (лог друкує викликач у безпечний момент).
void muteDac(bool mute, bool quiet = false) {
    digitalWrite(pins::kXsmt, mute ? cfg::kXsmtMutedLevel : cfg::kXsmtUnmutedLevel);
    if (mute != s_muted && !quiet) {
        LINE_LOG("[LINE] XSMT %s\n", mute ? "mute" : "unmute");
    }
    s_muted = mute;
}

// Повернути output-enable BCLK/WS (див. шапку файла); true — обидва справді «на виході».
bool restorePinOutputs() {
    gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sBclk));
    gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sWs));
    gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sBclk));
    gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sWs));
    const bool bclkOut = ((GPIO.enable >> pins::kI2sBclk) & 1u) != 0;
    const bool wsOut = ((GPIO.enable >> pins::kI2sWs) & 1u) != 0;
    return bclkOut && wsOut;
}

// GPIO18: true — на I2S1 TX (прохід), false — назад на I2S0 TX (радіо).
void routeDout(bool toLine) {
    const uint32_t sig = toLine ? I2S1O_SD_OUT_IDX : I2S0O_SD_OUT_IDX;
    gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sDout));
    esp_rom_gpio_connect_out_signal(pins::kI2sDout, sig, false, false);
    s_routed = toLine;
    LINE_LOG("[LINE] DOUT GPIO%d -> %s\n", pins::kI2sDout, toLine ? "I2S1 TX (line)" : "I2S0 TX (radio)");
}

int32_t dbToQ16(int8_t db) {
    return static_cast<int32_t>(powf(10.0f, static_cast<float>(db) / 20.0f) * kUnityQ16 + 0.5f);
}

// Нова ціль підсилення з лінійним переходом за rampMs.
void retarget(int32_t target, uint32_t rampMs) {
    s_gTgt = target;
    const int32_t frames = static_cast<int32_t>(cfg::kFsHz / 1000u * rampMs);
    int32_t step = (target - s_gCur) / (frames > 0 ? frames : 1);
    if (step == 0 && target != s_gCur) {
        step = (target > s_gCur) ? 1 : -1;
    }
    s_gStep = step;
}

bool IRAM_ATTR onRxOvf(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    s_rxOvf = s_rxOvf + 1;
    return false;
}

bool IRAM_ATTR onTxOvf(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    s_txOvf = s_txOvf + 1;
    return false;
}

void disableChannels() {
    if (s_txOn) {
        i2s_channel_disable(s_tx);
        s_txOn = false;
    }
    if (s_rxOn) {
        i2s_channel_disable(s_rx);
        s_rxOn = false;
    }
}

// Створити дуплексну пару каналів I2S1 (slave). Виконує ЗАДАЧА line_path (див. шапку). GPIO18 не чіпає.
esp_err_t createChannels() {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(cfg::kI2sPort),
                                                      I2S_ROLE_SLAVE);
    cc.dma_desc_num = cfg::kDmaDescCount;
    cc.dma_frame_num = cfg::kDmaFrameCount;
    cc.auto_clear = true;  // без даних TX шле нулі (захист від «повної гучності» / повтору буфера)
    esp_err_t e = i2s_new_channel(&cc, &s_tx, &s_rx);  // duplex на одному порту
    if (e != ESP_OK) {
        s_tx = nullptr;
        s_rx = nullptr;
        return e;
    }

    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg::kFsHz);  // у slave частоту задає зовнішній LRCLK
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;  // MCLK веде I2S0
    sc.gpio_cfg.bclk = static_cast<gpio_num_t>(pins::kI2sBclk);
    sc.gpio_cfg.ws = static_cast<gpio_num_t>(pins::kI2sWs);
    sc.gpio_cfg.din = static_cast<gpio_num_t>(pins::kI2sDin);
    sc.gpio_cfg.dout = I2S_GPIO_UNUSED;  // GPIO18 перемикаємо вручну (routeDout), не драйвером

    e = i2s_channel_init_std_mode(s_rx, &sc);
    if (e == ESP_OK) {
        e = i2s_channel_init_std_mode(s_tx, &sc);
    }
    if (e != ESP_OK) {
        i2s_del_channel(s_rx);
        i2s_del_channel(s_tx);
        s_rx = nullptr;
        s_tx = nullptr;
        restorePinOutputs();
        return e;
    }

    i2s_event_callbacks_t rxCb = {};
    rxCb.on_recv_q_ovf = onRxOvf;
    i2s_event_callbacks_t txCb = {};
    txCb.on_send_q_ovf = onTxOvf;
    i2s_channel_register_event_callback(s_rx, &rxCb, nullptr);
    i2s_channel_register_event_callback(s_tx, &txCb, nullptr);

    s_pinsOk = restorePinOutputs();
    return ESP_OK;
}

// Знищити канали (вимкнувши, якщо ще ввімкнені) і гарантувати, що I2S0 далі веде BCLK/WS.
void destroyChannels() {
    disableChannels();
    if (s_tx != nullptr) {
        i2s_del_channel(s_tx);
        s_tx = nullptr;
    }
    if (s_rx != nullptr) {
        i2s_del_channel(s_rx);
        s_rx = nullptr;
    }
    // Оборонно (НЕ перевірено, чи i2s_del_channel зачіпає ці піни): повернути output-enable й вихідні
    // сигнали I2S0 на BCLK/WS. Якщо вони вже такі — нічого не змінюється.
    restorePinOutputs();
    esp_rom_gpio_connect_out_signal(pins::kI2sBclk, I2S0O_BCK_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(pins::kI2sWs, I2S0O_WS_OUT_IDX, false, false);
}

void freeBuffer() {
    if (s_buf != nullptr) {
        heap_caps_free(s_buf);
        s_buf = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Обробка блоку
// ---------------------------------------------------------------------------

void processBlock(size_t frames) {
    int32_t* p = s_buf;
    for (size_t i = 0; i < frames; ++i) {
        // Пік входу (до послаблення) для діагностики: чи справді приходить сигнал.
        {
            const uint32_t al = (p[0] < 0) ? (0u - static_cast<uint32_t>(p[0])) : static_cast<uint32_t>(p[0]);
            const uint32_t ar = (p[1] < 0) ? (0u - static_cast<uint32_t>(p[1])) : static_cast<uint32_t>(p[1]);
            if (al > s_peakL) s_peakL = al;
            if (ar > s_peakR) s_peakR = ar;
        }
        if (s_gCur != s_gTgt) {
            s_gCur += s_gStep;
            if ((s_gStep > 0 && s_gCur >= s_gTgt) || (s_gStep < 0 && s_gCur <= s_gTgt) ||
                s_gStep == 0) {
                s_gCur = s_gTgt;
            }
        }
        p[0] = static_cast<int32_t>((static_cast<int64_t>(p[0]) * s_gCur) >> 16);
        p[1] = static_cast<int32_t>((static_cast<int64_t>(p[1]) * s_gCur) >> 16);
        p += 2;
    }
}

// Нема даних: перевіряємо аварійний мʼют.
void onNoData(uint32_t now) {
    const LinePathState st = s_state;
    if (st != LinePathState::Starting && st != LinePathState::Running) {
        return;
    }
    if (now - s_lastGoodMs < cfg::kStallMs) {
        return;
    }
    // Аварійний мʼют: підсилення 0 негайно (даних усе одно нема, TX шле нулі), XSMT low.
    s_gCur = 0;
    s_gTgt = 0;
    s_gStep = 0;
    muteDac(true);
    s_state = LinePathState::Stalled;
    s_stalls = s_stalls + 1;
    LINE_LOG("[LINE] STALL: no I2S1 RX blocks for %lu ms - muted (check PCM1808 / clocks)\n",
             static_cast<unsigned long>(now - s_lastGoodMs));
}

// Записати bytes байт із s_buf у TX.
void writeTx(size_t bytes) {
    size_t          wrote = 0;
    const esp_err_t we = i2s_channel_write(s_tx, s_buf, bytes, &wrote, cfg::kWriteTimeoutMs);
    if (we != ESP_OK || wrote != bytes) {
        s_writeTimeouts = s_writeTimeouts + 1;
    }
}

// Один цикл: читання -> (послаблення) -> запис. true — блок отримано.
bool pump() {
    // --- Ін'єкція збою (lp.fault): RX не споживається, DMA переповнюється, як при зависанні задачі ---
    if (s_faultUntilMs != 0) {
        if (static_cast<int32_t>(millis() - s_faultUntilMs) < 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            onNoData(millis());
            return false;
        }
        s_faultUntilMs = 0;
        LINE_LOG("[LINE] FAULT injection over (RX/TX overflows during it: rx +%lu, tx +%lu)\n",
                 static_cast<unsigned long>(s_rxOvf - s_faultRx0),
                 static_cast<unsigned long>(s_txOvf - s_faultTx0));
    }

    size_t        got = 0;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t e = i2s_channel_read(s_rx, s_buf, kBufBytes, &got, cfg::kReadTimeoutMs);
    const uint32_t waitUs = static_cast<uint32_t>(esp_timer_get_time() - t0);
    if (waitUs > s_maxReadWaitUs) s_maxReadWaitUs = waitUs;
    const uint32_t now = millis();

    if (e != ESP_OK || got < 2 * sizeof(int32_t)) {
        if (e == ESP_ERR_TIMEOUT) {
            s_readTimeouts = s_readTimeouts + 1;
        } else {
            s_readErrors = s_readErrors + 1;
            vTaskDelay(pdMS_TO_TICKS(10));  // не крутитись, якщо драйвер повертає помилку миттєво
        }
        onNoData(now);
        return false;
    }

    const uint32_t gapMs = now - s_lastGoodMs;
    s_lastGoodMs = now;
    bool logBack = false;
    if (s_state == LinePathState::Stalled) {
        s_state = LinePathState::Starting;
        s_settle = cfg::kSettleBlocks;
        s_recovering = true;
        logBack = true;
    }

    const size_t frames = got / (2 * sizeof(int32_t));
    const size_t bytes = frames * 2 * sizeof(int32_t);

    if (s_settle > 0) {
        // [P44b] Блок відкидаємо, але в TX пишемо нулі: канал TX увімкнений і має обслуговуватись.
        --s_settle;
        memset(s_buf, 0, bytes);
        writeTx(bytes);
        if (logBack) {
            LINE_LOG("[LINE] RX data is back after %lu ms - re-settling\n", static_cast<unsigned long>(gapMs));
        }
        return true;
    }

    // Лог переходу друкуємо ПІСЛЯ запису першого блоку (Serial.printf блокує на ≈5 мс на рядок).
    bool justStarted = false;
    bool justRecovered = false;
    if (s_state == LinePathState::Starting) {
        // Перший стабільний блок: знімаємо XSMT і плавно піднімаємо підсилення до цілі.
        s_gCur = 0;
        s_appliedAttDb = s_attDb;
        retarget(dbToQ16(s_appliedAttDb), cfg::kUnmuteRampMs);
        muteDac(false, true);
        s_state = LinePathState::Running;
        if (s_recovering) {
            s_recovering = false;
            s_recoveries = s_recoveries + 1;
            justRecovered = true;
        } else {
            s_starts = s_starts + 1;
            s_lastStartMs = millis() - s_startReqMs;
            justStarted = true;
        }
    } else if (s_state == LinePathState::Running && s_appliedAttDb != s_attDb) {
        s_appliedAttDb = s_attDb;
        retarget(dbToQ16(s_appliedAttDb), cfg::kUnmuteRampMs);
    }

    const int64_t t1 = esp_timer_get_time();
    processBlock(frames);
    writeTx(bytes);
    const uint32_t procUs = static_cast<uint32_t>(esp_timer_get_time() - t1);
    if (procUs > s_maxProcessUs) s_maxProcessUs = procUs;
    s_blocks = s_blocks + 1;

    if (justStarted) {
        LINE_LOG("[LINE] running: XSMT unmute, att %d dB, start took %lu ms%s\n",
                 static_cast<int>(s_appliedAttDb), static_cast<unsigned long>(s_lastStartMs),
                 s_pinsOk ? "" : ", WARNING BCLK/WS output enable MISSING");
    } else if (justRecovered) {
        LINE_LOG("[LINE] recovered: XSMT unmute, att %d dB\n", static_cast<int>(s_appliedAttDb));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Послідовності
// ---------------------------------------------------------------------------

// Зняти фіксацію 48 кГц (лише при Idle радіо; бібліотека не потокобезпечна). Без повернення помилки:
// лише лог, як у P44.
void releaseFixedRate() {
    if (!AudioPlayer::setFixedOutputRate(false)) {
        LINE_LOG("[LINE] radio: player refused to release fixed rate (not Idle?)\n");
        return;
    }
    const uint32_t c0 = millis();
    while (AudioPlayer::fixedOutputRate()) {
        if (millis() - c0 > cfg::kClockApplyWaitMs) {
            LINE_LOG("[LINE] radio: fixed rate not released\n");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void doStop() {
    s_faultUntilMs = 0;  // зупинка скасовує ін'єкцію збою
    if (!s_rxOn && !s_txOn) {
        s_state = LinePathState::Off;
        return;
    }
    s_state = LinePathState::Stopping;
    retarget(0, cfg::kMuteRampMs);
    const uint32_t t0 = millis();
    while (s_gCur != 0 && millis() - t0 < cfg::kMuteRampMs * 3 + cfg::kReadTimeoutMs) {
        pump();
    }
    s_gCur = s_gTgt = s_gStep = 0;
    muteDac(true);
    // [P44b] Вікно після XSMT low: канали ще увімкнені, тож RX вичитуємо, а в TX пишемо нулі
    // (підсилення 0) — ні RX, ні TX не стоять без обслуговування довше за глибину черги DMA.
    const uint32_t w0 = millis();
    do {
        pump();
    } while (millis() - w0 < cfg::kXsmtMuteSettleMs);
    disableChannels();
    s_state = LinePathState::Off;
    LINE_LOG("[LINE] stopped (DOUT %s)\n", s_routed ? "still on I2S1" : "on I2S0");
}

// Повернення до радіо + звільнення ресурсів. Після неї задача завершується (s_exit).
void doRadio() {
    if (s_state != LinePathState::Off) {
        doStop();
    }
    muteDac(true);
    routeDout(false);
    releaseFixedRate();
    destroyChannels();
    freeBuffer();
    s_exit = true;
    LINE_LOG("[LINE] radio path restored, line path resources released "
             "(XSMT stays low until the player unmutes)\n");
}

// Невдача під час start() ПІСЛЯ того, як запит прийнято: відкат як returnToRadio().
void abortStart(const char* why) {
    LINE_LOG("[LINE] start failed: %s - rolling back to radio path\n", why);
    doRadio();
}

void doStart() {
    muteDac(true);
    s_gCur = s_gTgt = s_gStep = 0;
    s_recovering = false;

    // 1. Радіо: зупинити й дати бібліотеці дограти хвіст буфера (його чути не буде, але зміну
    //    тактів не можна робити під час запису в I2S0).
    AudioPlayer::stop();
    const uint32_t w0 = millis();
    while (AudioPlayer::state() != PlayerState::Idle) {
        if (millis() - w0 > cfg::kRadioStopWaitMs) {
            char why[64];
            snprintf(why, sizeof(why), "radio did not reach Idle (state %s)",
                     AudioPlayer::stateName(AudioPlayer::state()));
            abortStart(why);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::kRadioDrainMs));

    // 2. Зафіксувати 48 кГц на I2S0 (виконує задача плеєра; лише при Idle).
    if (!AudioPlayer::setFixedOutputRate(true)) {
        abortStart("player refused fixed output rate");
        return;
    }
    const uint32_t c0 = millis();
    while (!AudioPlayer::fixedOutputRate()) {
        if (millis() - c0 > cfg::kClockApplyWaitMs) {
            abortStart("48 kHz not applied on I2S0");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelay(pdMS_TO_TICKS(20));  // такти після reconfig стабілізуються

    // 3. DOUT -> I2S1 TX (під мʼютом).
    routeDout(true);

    // 4. Канали: RX першим, TX другим. Після enable повертаємо output-enable BCLK/WS.
    //    Жодного логу між enable і першим pump() (див. шапку): друкуємо підсумок у рядку "running".
    esp_err_t e = i2s_channel_enable(s_rx);
    if (e != ESP_OK) {
        char why[64];
        snprintf(why, sizeof(why), "RX enable: %s", esp_err_to_name(e));
        abortStart(why);
        return;
    }
    s_rxOn = true;
    e = i2s_channel_enable(s_tx);
    if (e != ESP_OK) {
        char why[64];
        snprintf(why, sizeof(why), "TX enable: %s", esp_err_to_name(e));
        disableChannels();
        abortStart(why);
        return;
    }
    s_txOn = true;
    s_pinsOk = restorePinOutputs();

    s_settle = cfg::kSettleBlocks;
    s_lastGoodMs = millis();
    s_state = LinePathState::Starting;
}

// Тіло задачі. Перша дія — створення каналів і сигнал start(); завершується сама після doRadio().
void taskEntry(void*) {
    const esp_err_t e = createChannels();
    if (e != ESP_OK || s_initAbandoned) {
        const bool abandoned = s_initAbandoned;
        if (e == ESP_OK) {
            destroyChannels();
        }
        freeBuffer();
        s_req = kReqNone;
        s_initErr = static_cast<int>(e);
        s_stackFreeMin = uxTaskGetStackHighWaterMark(nullptr);
        s_task = nullptr;
        s_taskAlive = false;
        if (!abandoned) {
            xSemaphoreGive(s_initSem);
        }
        vTaskDelete(nullptr);
        return;
    }
    s_initErr = 0;
    xSemaphoreGive(s_initSem);

    for (;;) {
        const uint8_t r = s_req;
        s_req = kReqNone;
        if (r == kReqStart) {
            if (s_state == LinePathState::Off) doStart();
        } else if (r == kReqStop) {
            doStop();
        } else if (r == kReqRadio) {
            doRadio();
        }

        if (s_exit) {
            s_exit = false;
            s_stackFreeMin = uxTaskGetStackHighWaterMark(nullptr);
            s_task = nullptr;
            s_taskAlive = false;  // останній запис: після нього новий start() може створити нову задачу
            vTaskDelete(nullptr);
            return;
        }

        if (s_state == LinePathState::Off) {
            vTaskDelay(pdMS_TO_TICKS(cfg::kIdlePollMs));
        } else {
            pump();
        }
    }
}

// start(), частина під s_opLock. Див. line_path.h.
bool startLocked() {
    if (s_req != kReqNone) {
        return false;
    }
    if (s_taskAlive) {
        // Ресурси є (після stop()): лише новий запит.
        if (s_state != LinePathState::Off) {
            return false;
        }
        s_startReqMs = millis();
        muteDac(true);
        s_req = kReqStart;
        return true;
    }
    if (s_state != LinePathState::Off) {
        return false;
    }

    // Ресурсів нема: створюємо. XSMT low першим — що б далі не сталось, звуку не буде.
    s_startReqMs = millis();
    muteDac(true);
    s_heapFreeBefore = static_cast<uint32_t>(heapFree());
    s_heapLargestBefore = static_cast<uint32_t>(heapLargest());

    s_buf = static_cast<int32_t*>(heap_caps_malloc(kBufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_buf == nullptr) {
        s_allocFailures = s_allocFailures + 1;
        LINE_LOG("[LINE] start rejected: no memory for block buffer (%u B); free=%u B, largest=%u B\n",
                 static_cast<unsigned>(kBufBytes), static_cast<unsigned>(heapFree()),
                 static_cast<unsigned>(heapLargest()));
        return false;
    }

    s_initAbandoned = false;
    s_initErr = 0;
    s_exit = false;
    xSemaphoreTake(s_initSem, 0);  // скинути можливий застарілий сигнал
    s_req = kReqStart;
    s_taskAlive = true;
    const BaseType_t ok = xTaskCreatePinnedToCore(taskEntry, "line_path", cfg::kTaskStackBytes, nullptr,
                                                  cfg::kTaskPriority, &s_task, cfg::kTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        s_taskAlive = false;
        s_req = kReqNone;
        freeBuffer();
        s_allocFailures = s_allocFailures + 1;
        LINE_LOG("[LINE] start rejected: task create failed (stack %u B); free=%u B, largest=%u B\n",
                 static_cast<unsigned>(cfg::kTaskStackBytes), static_cast<unsigned>(heapFree()),
                 static_cast<unsigned>(heapLargest()));
        return false;
    }

    // Чекаємо, поки задача створить канали I2S1 (≈10–20 мс).
    if (xSemaphoreTake(s_initSem, pdMS_TO_TICKS(cfg::kCreateWaitMs)) != pdTRUE &&
        xSemaphoreTake(s_initSem, 0) != pdTRUE) {
        s_initAbandoned = true;  // задача сама згорнеться, коли дійде до цього місця
        s_allocFailures = s_allocFailures + 1;
        LINE_LOG("[LINE] start rejected: resource creation timed out after %lu ms\n",
                 static_cast<unsigned long>(cfg::kCreateWaitMs));
        return false;
    }
    if (s_initErr != 0) {
        s_allocFailures = s_allocFailures + 1;
        LINE_LOG("[LINE] start rejected: I2S1 channels not created (%s); XSMT low, DOUT stays on I2S0; "
                 "free=%u B, largest=%u B\n",
                 esp_err_to_name(static_cast<esp_err_t>(s_initErr)), static_cast<unsigned>(heapFree()),
                 static_cast<unsigned>(heapLargest()));
        return false;
    }

    s_heapFreeAfter = static_cast<uint32_t>(heapFree());
    s_heapLargestAfter = static_cast<uint32_t>(heapLargest());
    LINE_LOG("[LINE] resources up: used %d B internal RAM (free %u -> %u B, largest %u -> %u B)\n",
             static_cast<int>(s_heapFreeBefore) - static_cast<int>(s_heapFreeAfter),
             static_cast<unsigned>(s_heapFreeBefore), static_cast<unsigned>(s_heapFreeAfter),
             static_cast<unsigned>(s_heapLargestBefore), static_cast<unsigned>(s_heapLargestAfter));
    return true;
}

// ---------------------------------------------------------------------------
// Доступ для Serial-тесту
// ---------------------------------------------------------------------------

[[maybe_unused]] DbgInfo dbgInfo() {
    DbgInfo d = {};
    d.alive = s_taskAlive;
    const TaskHandle_t t = s_task;
    d.stackFreeMin = (d.alive && t != nullptr) ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(t))
                                                : s_stackFreeMin;
    d.heapFreeBefore = s_heapFreeBefore;
    d.heapLargestBefore = s_heapLargestBefore;
    d.heapFreeAfter = s_heapFreeAfter;
    d.heapLargestAfter = s_heapLargestAfter;
    d.recoveries = s_recoveries;
    return d;
}

// Запустити ін'єкцію збою на ms мілісекунд. Лише з Running.
[[maybe_unused]] bool dbgFault(uint32_t ms) {
    if (s_state != LinePathState::Running || s_faultUntilMs != 0) {
        return false;
    }
    s_faultRx0 = s_rxOvf;
    s_faultTx0 = s_txOvf;
    uint32_t until = millis() + ms;
    if (until == 0) until = 1;
    s_faultUntilMs = until;
    return true;
}

}  // namespace

#else  // LINE_PATH_ENABLE

namespace {
[[maybe_unused]] DbgInfo dbgInfo() { return DbgInfo{}; }
[[maybe_unused]] bool dbgFault(uint32_t) { return false; }
}  // namespace

#endif  // LINE_PATH_ENABLE

// ===========================================================================
// Serial-тест (працює і при LINE_PATH_ENABLE == 0: тоді лише lp.heap)
// ===========================================================================
namespace {

#if LINE_PATH_TEST

void printHeap() {
    Serial.printf("[LINE] heap internal: free=%u B, largest block=%u B, min free ever=%u B "
                  "(LINE_PATH_ENABLE=%d)\n",
                  static_cast<unsigned>(heapFree()), static_cast<unsigned>(heapLargest()),
                  static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                  LINE_PATH_ENABLE);
}

#if LINE_PATH_ENABLE

void printStats() {
    const LinePathStats s = LinePath::stats();
    const DbgInfo       d = dbgInfo();
    Serial.printf("[LINE] state=%s muted=%d dout=%s att=%d dB resources=%s\n", LinePath::stateName(s.state),
                  s.muted ? 1 : 0, s.routedToLine ? "I2S1(line)" : "I2S0(radio)",
                  static_cast<int>(s.attenuationDb), d.alive ? "up" : "down");
    Serial.printf("[LINE] blocks=%lu starts=%lu stalls=%lu recoveries=%lu readTimeouts=%lu readErrors=%lu\n",
                  static_cast<unsigned long>(s.blocks), static_cast<unsigned long>(s.starts),
                  static_cast<unsigned long>(s.stalls), static_cast<unsigned long>(d.recoveries),
                  static_cast<unsigned long>(s.readTimeouts), static_cast<unsigned long>(s.readErrors));
    Serial.printf("[LINE] writeTimeouts=%lu rxOvf=%lu txOvf=%lu allocFailures=%lu maxProcess=%lu us "
                  "maxReadWait=%lu us\n",
                  static_cast<unsigned long>(s.writeTimeouts), static_cast<unsigned long>(s.rxOverflows),
                  static_cast<unsigned long>(s.txOverflows), static_cast<unsigned long>(s.allocFailures),
                  static_cast<unsigned long>(s.maxProcessUs), static_cast<unsigned long>(s.maxReadWaitUs));
    Serial.printf("[LINE] input peak L %.1f dBFS, R %.1f dBFS (since lp.reset)\n",
                  static_cast<double>(s.peakLeftDbfs), static_cast<double>(s.peakRightDbfs));
    Serial.printf("[LINE] initMs=%lu lastStartMs=%lu stackFreeMin=%u B%s\n",
                  static_cast<unsigned long>(s.initMs), static_cast<unsigned long>(s.lastStartMs),
                  static_cast<unsigned>(d.stackFreeMin), d.alive ? "" : " (last run)");
    Serial.printf("[LINE] last create: free %u -> %u B, largest %u -> %u B\n",
                  static_cast<unsigned>(d.heapFreeBefore), static_cast<unsigned>(d.heapFreeAfter),
                  static_cast<unsigned>(d.heapLargestBefore), static_cast<unsigned>(d.heapLargestAfter));
    printHeap();
}

// lp.fault [мс]: зупинити споживання RX на ms і простежити Running -> Stalled -> Starting -> Running.
void cmdFault(uint32_t ms) {
    if (ms <= cfg::kStallMs || ms > cfg::kFaultMaxMs) {
        Serial.printf("[LINE] fault: duration must be > kStallMs (%lu) and <= %lu ms\n",
                      static_cast<unsigned long>(cfg::kStallMs), static_cast<unsigned long>(cfg::kFaultMaxMs));
        return;
    }
    if (!dbgFault(ms)) {
        Serial.println("[LINE] fault rejected: state must be Running with no fault active (lp.start first)");
        return;
    }
    Serial.printf("[LINE] fault: RX blocks not consumed for %lu ms (stall threshold %lu ms)\n",
                  static_cast<unsigned long>(ms), static_cast<unsigned long>(cfg::kStallMs));

    const uint32_t t0 = millis();
    const uint32_t deadline = ms + cfg::kFaultRecoverWaitMs;
    LinePathState  last = LinePathState::Running;
    bool           sawStalled = false;
    bool           mutedInStall = false;
    bool           recovered = false;
    while (millis() - t0 < deadline) {
        const LinePathStats s = LinePath::stats();
        if (s.state != last) {
            Serial.printf("[LINE] fault t=%lu ms: %s -> %s (muted=%d)\n", static_cast<unsigned long>(millis() - t0),
                          LinePath::stateName(last), LinePath::stateName(s.state), s.muted ? 1 : 0);
            last = s.state;
        }
        if (s.state == LinePathState::Stalled) {
            sawStalled = true;
            if (s.muted) mutedInStall = true;
        }
        if (sawStalled && s.state == LinePathState::Running && !s.muted) {
            recovered = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    const LinePathStats f = LinePath::stats();
    Serial.printf("[LINE] fault result: stalled=%s mutedInStall=%s recovered=%s -> %s (stalls=%lu, "
                  "rxOvf=%lu txOvf=%lu; overflows during the injected fault are expected)\n",
                  sawStalled ? "yes" : "NO", mutedInStall ? "yes" : "NO", recovered ? "yes" : "NO",
                  (sawStalled && mutedInStall && recovered) ? "PASS" : "FAIL",
                  static_cast<unsigned long>(f.stalls), static_cast<unsigned long>(f.rxOverflows),
                  static_cast<unsigned long>(f.txOverflows));
}

// Дочекатися умови з таймаутом.
bool waitState(LinePathState want, uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (LinePath::state() != want) {
        if (millis() - t0 > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool waitTaskGone(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (dbgInfo().alive) {
        if (millis() - t0 > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// lp.cycle <N>: N разів start -> пауза -> returnToRadio, друк free / largest після кожного.
void cmdCycle(int n) {
    if (n < 1 || n > static_cast<int>(cfg::kCycleMaxCount)) {
        Serial.printf("[LINE] cycle: N must be 1..%lu\n", static_cast<unsigned long>(cfg::kCycleMaxCount));
        return;
    }
    if (LinePath::state() != LinePathState::Off || dbgInfo().alive) {
        Serial.println("[LINE] cycle rejected: line path must be Off with resources released (lp.radio first)");
        return;
    }
    if (AudioPlayer::state() != PlayerState::Idle) {
        Serial.println("[LINE] cycle rejected: radio is not Idle (switch to an external input / stop it first)");
        return;
    }

    const LinePathStats s0 = LinePath::stats();
    const int           baseFree = static_cast<int>(heapFree());
    const int           baseLargest = static_cast<int>(heapLargest());
    uint32_t            prevRx = s0.rxOverflows;
    uint32_t            prevTx = s0.txOverflows;
    int                 minLargest = baseLargest;
    int                 lastFree = baseFree;
    int                 lastLargest = baseLargest;
    uint32_t            sumRx = 0;
    uint32_t            sumTx = 0;
    int                 done = 0;
    Serial.printf("[LINE] cycle x%d: baseline (Idle, resources down) free=%d B, largest=%d B, rxOvf=%lu, txOvf=%lu\n",
                  n, baseFree, baseLargest, static_cast<unsigned long>(prevRx), static_cast<unsigned long>(prevTx));

    for (int i = 1; i <= n; ++i) {
        if (!LinePath::start()) {
            Serial.printf("[LINE] cycle %d: start() rejected (allocFailures=%lu) - abort\n", i,
                          static_cast<unsigned long>(LinePath::stats().allocFailures));
            break;
        }
        if (!waitState(LinePathState::Running, cfg::kCycleStartWaitMs)) {
            Serial.printf("[LINE] cycle %d: not Running within %lu ms (state %s) - abort\n", i,
                          static_cast<unsigned long>(cfg::kCycleStartWaitMs),
                          LinePath::stateName(LinePath::state()));
            LinePath::returnToRadio();
            waitTaskGone(cfg::kCycleRadioWaitMs);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(cfg::kCycleRunMs));
        const int upFree = static_cast<int>(heapFree());
        const int upLargest = static_cast<int>(heapLargest());

        LinePath::returnToRadio();
        const bool gone = waitTaskGone(cfg::kCycleRadioWaitMs);
        vTaskDelay(pdMS_TO_TICKS(cfg::kCycleFreeSettleMs));  // IDLE-задача звільняє стек і TCB
        const int           f = static_cast<int>(heapFree());
        const int           l = static_cast<int>(heapLargest());
        const LinePathStats s = LinePath::stats();
        const uint32_t      dRx = s.rxOverflows - prevRx;
        const uint32_t      dTx = s.txOverflows - prevTx;
        prevRx = s.rxOverflows;
        prevTx = s.txOverflows;
        sumRx += dRx;
        sumTx += dTx;
        if (l < minLargest) minLargest = l;
        lastFree = f;
        lastLargest = l;
        ++done;
        Serial.printf("[LINE] cycle %d/%d: up free=%d largest=%d | down free=%d largest=%d "
                      "(vs baseline: free %+d, largest %+d) | rx+%lu tx+%lu stalls=%lu%s\n",
                      i, n, upFree, upLargest, f, l, f - baseFree, l - baseLargest,
                      static_cast<unsigned long>(dRx), static_cast<unsigned long>(dTx),
                      static_cast<unsigned long>(s.stalls), gone ? "" : " WARNING: task still alive");
        if (!gone) break;
    }
    Serial.printf("[LINE] cycle summary: %d done; free %d -> %d B (%+d), largest %d -> %d B (%+d), min largest %d B; "
                  "overflows on transitions: rx +%lu, tx +%lu; allocFailures=%lu\n",
                  done, baseFree, lastFree, lastFree - baseFree, baseLargest, lastLargest, lastLargest - baseLargest,
                  minLargest, static_cast<unsigned long>(sumRx), static_cast<unsigned long>(sumTx),
                  static_cast<unsigned long>(LinePath::stats().allocFailures));
}

#endif  // LINE_PATH_ENABLE

void handleLine(const char* line) {
    if (strncmp(line, "lp.", 3) != 0) {
        return;  // не наша команда (Serial можуть читати й інші тести)
    }
    const char* c = line + 3;
    if (strcmp(c, "heap") == 0) {
        printHeap();
        return;
    }
#if LINE_PATH_ENABLE
    if (strcmp(c, "start") == 0) {
        Serial.println(LinePath::start() ? "[LINE] start requested" : "[LINE] start rejected");
    } else if (strcmp(c, "stop") == 0) {
        Serial.println(LinePath::stop() ? "[LINE] stop requested" : "[LINE] stop rejected");
    } else if (strcmp(c, "radio") == 0) {
        Serial.println(LinePath::returnToRadio() ? "[LINE] radio requested" : "[LINE] radio rejected");
    } else if (strcmp(c, "stat") == 0) {
        printStats();
    } else if (strncmp(c, "att ", 4) == 0) {
        const int v = atoi(c + 4);
        if (v < cfg::kAttMinDb || v > cfg::kAttMaxDb) {
            Serial.printf("[LINE] att out of range (%d..%d dB)\n", static_cast<int>(cfg::kAttMinDb),
                          static_cast<int>(cfg::kAttMaxDb));
        } else {
            LinePath::setAttenuationDb(static_cast<int8_t>(v));
            Serial.printf("[LINE] att = %d dB\n", v);
        }
    } else if (strcmp(c, "reset") == 0) {
        LinePath::resetStats();
        Serial.println("[LINE] stats reset");
    } else if (strcmp(c, "fault") == 0) {
        cmdFault(cfg::kFaultDefaultMs);
    } else if (strncmp(c, "fault ", 6) == 0) {
        const int v = atoi(c + 6);
        cmdFault(v > 0 ? static_cast<uint32_t>(v) : 0u);
    } else if (strncmp(c, "cycle ", 6) == 0) {
        cmdCycle(atoi(c + 6));
    } else {
        Serial.println("[LINE] commands: lp.start lp.stop lp.radio lp.stat lp.reset lp.att <dB> "
                       "lp.fault [ms] lp.cycle <N> lp.heap");
    }
#else
    Serial.println("[LINE] line path disabled (LINE_PATH_ENABLE = 0): only lp.heap is available");
#endif
}

void testTask(void*) {
    char   line[cfg::kTestLineMax];
    size_t n = 0;
    for (;;) {
        while (Serial.available() > 0) {
            const int ch = Serial.read();
            if (ch == '\r' || ch == '\n') {
                if (n > 0) {
                    line[n] = '\0';
                    handleLine(line);
                    n = 0;
                }
            } else if (n + 1 < sizeof(line)) {
                line[n++] = static_cast<char>(ch);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(cfg::kTestPollMs));
    }
}

// Створити задачу Serial-тесту (один раз). Резидентна: ≈4 КБ стека — однакові при LINE_PATH_ENABLE 0 і 1.
void startTestTask() {
    static bool started = false;
    if (started) {
        return;
    }
    TaskHandle_t testHandle = nullptr;
    if (xTaskCreatePinnedToCore(testTask, "line_test", cfg::kTestTaskStackBytes, nullptr,
                                cfg::kTestTaskPriority, &testHandle, cfg::kTestTaskCore) == pdPASS) {
        started = true;
        Serial.println("[LINE] test mode ready (send 'lp.help')");
    } else {
        Serial.println("[LINE] test task create failed");
    }
}

#else  // LINE_PATH_TEST

[[maybe_unused]] void startTestTask() {}

#endif  // LINE_PATH_TEST

}  // namespace

// ===========================================================================
// Публічний інтерфейс
// ===========================================================================

#if LINE_PATH_ENABLE

bool LinePath::begin() {
    if (s_ready) {
        return true;
    }
    const uint32_t t0 = millis();
    s_initSem = xSemaphoreCreateBinaryStatic(&s_initSemBuf);
    if (s_initSem == nullptr) {
        Serial.println("[LINE] begin failed: semaphore");
        return false;
    }
    startTestTask();
    s_initMs = millis() - t0;
    s_ready = true;
    LINE_LOG("[LINE] ready (I2S1 channels, task and buffers are created on start()); "
             "internal RAM free=%u B, largest block=%u B\n",
             static_cast<unsigned>(heapFree()), static_cast<unsigned>(heapLargest()));
    return true;
}

bool LinePath::isReady() {
    return s_ready;
}

bool LinePath::start() {
    if (!s_ready || __atomic_exchange_n(&s_opLock, true, __ATOMIC_ACQ_REL)) {
        return false;
    }
    const bool ok = startLocked();
    __atomic_store_n(&s_opLock, false, __ATOMIC_RELEASE);
    return ok;
}

bool LinePath::stop() {
    if (!s_ready || __atomic_exchange_n(&s_opLock, true, __ATOMIC_ACQ_REL)) {
        return false;
    }
    bool ok = false;
    if (s_taskAlive && s_req == kReqNone) {
        s_req = kReqStop;
        ok = true;
    }
    __atomic_store_n(&s_opLock, false, __ATOMIC_RELEASE);
    return ok;
}

bool LinePath::returnToRadio() {
    if (!s_ready || __atomic_exchange_n(&s_opLock, true, __ATOMIC_ACQ_REL)) {
        return false;
    }
    bool ok = false;
    if (s_req == kReqNone) {
        ok = true;
        if (s_taskAlive) {
            s_req = kReqRadio;
        } else {
            // Ресурсів уже нема, GPIO18 на I2S0 (так лишає кожен шлях виходу). Лише страхуємося, що
            // фіксацію 48 кГц знято (асинхронно, лише при Idle радіо).
            LINE_LOG("[LINE] radio: resources already released\n");
            if (AudioPlayer::fixedOutputRate()) {
                AudioPlayer::setFixedOutputRate(false);
            }
        }
    }
    __atomic_store_n(&s_opLock, false, __ATOMIC_RELEASE);
    return ok;
}

bool LinePath::isRunning() {
    const LinePathState st = s_state;
    return st == LinePathState::Starting || st == LinePathState::Running ||
           st == LinePathState::Stalled;
}

LinePathState LinePath::state() {
    return s_state;
}

const char* LinePath::stateName(LinePathState s) {
    switch (s) {
        case LinePathState::Off:      return "Off";
        case LinePathState::Starting: return "Starting";
        case LinePathState::Running:  return "Running";
        case LinePathState::Stalled:  return "Stalled";
        case LinePathState::Stopping: return "Stopping";
    }
    return "?";
}

void LinePath::setAttenuationDb(int8_t db) {
    if (db < cfg::kAttMinDb) db = cfg::kAttMinDb;
    if (db > cfg::kAttMaxDb) db = cfg::kAttMaxDb;
    s_attDb = db;
}

int8_t LinePath::attenuationDb() {
    return s_attDb;
}

LinePathStats LinePath::stats() {
    LinePathStats s = {};
    s.state = s_state;
    s.muted = s_muted;
    s.routedToLine = s_routed;
    s.attenuationDb = s_attDb;
    s.blocks = s_blocks;
    s.readTimeouts = s_readTimeouts;
    s.readErrors = s_readErrors;
    s.writeTimeouts = s_writeTimeouts;
    s.rxOverflows = s_rxOvf;
    s.txOverflows = s_txOvf;
    s.stalls = s_stalls;
    s.starts = s_starts;
    s.maxProcessUs = s_maxProcessUs;
    s.maxReadWaitUs = s_maxReadWaitUs;
    s.initMs = s_initMs;
    s.lastStartMs = s_lastStartMs;
    s.peakLeftDbfs = (s_peakL > 0) ? 20.0f * log10f(static_cast<float>(s_peakL) / 2147483648.0f) : -99.0f;
    s.peakRightDbfs = (s_peakR > 0) ? 20.0f * log10f(static_cast<float>(s_peakR) / 2147483648.0f) : -99.0f;
    s.allocFailures = s_allocFailures;
    return s;
}

void LinePath::resetStats() {
    s_blocks = s_readTimeouts = s_readErrors = s_writeTimeouts = 0;
    s_stalls = s_starts = s_maxProcessUs = s_maxReadWaitUs = 0;
    s_allocFailures = s_recoveries = 0;
    s_rxOvf = 0;
    s_txOvf = 0;
    s_peakL = 0;
    s_peakR = 0;
}

#else  // LINE_PATH_ENABLE

// Модуль вимкнено: лише Serial-тест (lp.heap) для еталонного заміру RAM без line_path.
bool LinePath::begin() {
    startTestTask();
    return false;
}
bool LinePath::isReady() { return false; }
bool LinePath::start() { return false; }
bool LinePath::stop() { return false; }
bool LinePath::returnToRadio() { return false; }
bool LinePath::isRunning() { return false; }
LinePathState LinePath::state() { return LinePathState::Off; }
const char* LinePath::stateName(LinePathState) { return "Off"; }
void LinePath::setAttenuationDb(int8_t) {}
int8_t LinePath::attenuationDb() { return line_path_cfg::kPrototypeAttenuationDb; }
LinePathStats LinePath::stats() { return LinePathStats{}; }
void LinePath::resetStats() {}

#endif  // LINE_PATH_ENABLE
