// Прототип проходу зовнішнього звуку PCM1808 -> ESP32-S3 -> PCM5102 (Prompt 44). Опис — audio/line_path.h.
//
// Топологія (V2 з P43):
//   PCM1808 --DIN GPIO8--> I2S1 RX (slave) --[блок 128 кадрів, послаблення]--> I2S1 TX (slave)
//   --DOUT--> GPIO18 --> PCM5102. BCLK GPIO15 / WS GPIO17 веде I2S0 бібліотеки ESP32-audioI2S
//   (master, MCLK GPIO16 = 256·fs для PCM1808); I2S1 їх лише слухає, тож RX і TX синхронні
//   й звідти ж береться швидкість (ніякого ресемплінгу чи дрейфу між АЦП і ЦАП).
//
// GPIO18 фізично одна: у радіо-режимі її веде I2S0 (ESP32-audioI2S, setPinout), у режимі проходу —
// I2S1 TX. Перемикання — лише GPIO-матрицею (esp_rom_gpio_connect_out_signal), без зміни
// бібліотеки. Канал TX ініціалізується з dout = UNUSED, щоб драйвер IDF не перехопив GPIO18 під час
// begin() (інакше радіо замовкло б одразу).
//
// ВАЖЛИВО про піни 15/17: вони водночас вихід I2S0 і вхід I2S1 (slave). Драйвер IDF при
// ініціалізації slave-входів може лишити пін «лише входом» — після init/enable повертаємо
// output-enable низькорівнево (gpio_ll), як це робить audio/adc_vu (P37). gpio_set_direction()
// не використовуємо: він може відключити вихідний сигнал піна від I2S0.
//
// Безпека: старт завжди під XSMT low, підсилення 0; мʼют знімається лише після kSettleBlocks
// стабільних блоків; при відсутності блоків понад kStallMs — XSMT low і підсилення 0 (TX має
// auto_clear, тож без даних у лінію йдуть нулі, а не «повна гучність»).
//
// Задача line_path — єдиний «власник» I2S1 і XSMT на час проходу. Прототип: XSMT керується тут
// напряму (digitalWrite), у P47 це перейде до dac_output.

#include "audio/line_path.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config/line_path_config.h"

#if LINE_PATH_ENABLE

#include "audio/audio_player.h"
#include "config/audio_player_config.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"

static_assert(line_path_cfg::kXsmtMutedLevel == player_cfg::kXsmtMutedLevel &&
                  line_path_cfg::kXsmtUnmutedLevel == player_cfg::kXsmtUnmutedLevel,
              "XSMT levels must match player_cfg");

#if LINE_PATH_DEBUG
#define LINE_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LINE_LOG(...) \
    do {              \
    } while (0)
#endif

namespace {

namespace cfg = line_path_cfg;

enum Req : uint8_t { kReqNone = 0, kReqStart, kReqStop, kReqRadio };

constexpr int32_t kUnityQ16 = 65536;  // 1.0 у форматі Q16

// --- Стан (пише лише задача, крім s_req / s_attDb / лічильників ISR) ---
volatile uint8_t       s_req = kReqNone;
volatile LinePathState s_state = LinePathState::Off;
volatile bool          s_muted = true;
volatile bool          s_routed = false;
volatile int8_t        s_attDb = cfg::kPrototypeAttenuationDb;
volatile uint32_t      s_rxOvf = 0;
volatile uint32_t      s_txOvf = 0;

bool              s_ready = false;
bool              s_rxOn = false;
bool              s_txOn = false;
i2s_chan_handle_t s_rx = nullptr;
i2s_chan_handle_t s_tx = nullptr;
TaskHandle_t      s_task = nullptr;

// Інтерліврований L,R,L,R...; 24 біти даних вирівняні вліво в 32-бітному слові (домен хука).
int32_t s_buf[cfg::kBlockFrames * 2];

// Підсилення: Q16, лінійний перехід по кадрах.
int32_t s_gCur = 0;
int32_t s_gTgt = 0;
int32_t s_gStep = 0;
int8_t  s_appliedAttDb = cfg::kPrototypeAttenuationDb;

uint32_t s_settle = 0;
uint32_t s_lastGoodMs = 0;
uint32_t s_startReqMs = 0;
bool     s_stallLogged = false;

// Статистика (читається з інших задач без блокувань; значення 32-бітні, вирівняні).
uint32_t s_blocks = 0, s_readTimeouts = 0, s_readErrors = 0, s_writeTimeouts = 0;
uint32_t s_stalls = 0, s_starts = 0, s_maxProcessUs = 0, s_maxReadWaitUs = 0;
uint32_t s_initMs = 0, s_lastStartMs = 0;
uint32_t s_peakL = 0, s_peakR = 0;  // |x| у шкалі 2^31, макс. з останнього resetStats()
int      s_heapUsedByBegin = 0;  // внутрішня RAM, яку забрав begin() (діагностика)

// ---------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------

void muteDac(bool mute) {
    digitalWrite(pins::kXsmt, mute ? cfg::kXsmtMutedLevel : cfg::kXsmtUnmutedLevel);
    if (mute != s_muted) {
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
    ++s_stalls;
    LINE_LOG("[LINE] STALL: no I2S1 RX blocks for %lu ms - muted (check PCM1808 / clocks)\n",
             static_cast<unsigned long>(now - s_lastGoodMs));
}

// Один цикл: читання -> (послаблення) -> запис. true — блок отримано.
bool pump() {
    size_t        got = 0;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t e = i2s_channel_read(s_rx, s_buf, sizeof(s_buf), &got, cfg::kReadTimeoutMs);
    const uint32_t waitUs = static_cast<uint32_t>(esp_timer_get_time() - t0);
    if (waitUs > s_maxReadWaitUs) s_maxReadWaitUs = waitUs;
    const uint32_t now = millis();

    if (e != ESP_OK || got < 2 * sizeof(int32_t)) {
        if (e == ESP_ERR_TIMEOUT) {
            ++s_readTimeouts;
        } else {
            ++s_readErrors;
            vTaskDelay(pdMS_TO_TICKS(10));  // не крутитись, якщо драйвер повертає помилку миттєво
        }
        onNoData(now);
        return false;
    }

    s_lastGoodMs = now;
    if (s_state == LinePathState::Stalled) {
        s_state = LinePathState::Starting;
        s_settle = cfg::kSettleBlocks;
        LINE_LOG("[LINE] RX data is back - re-settling\n");
    }

    if (s_settle > 0) {
        --s_settle;  // у TX нічого не пишемо: auto_clear шле нулі
        return true;
    }

    if (s_state == LinePathState::Starting) {
        // Перший стабільний блок: знімаємо XSMT і плавно піднімаємо підсилення до цілі.
        s_gCur = 0;
        s_appliedAttDb = s_attDb;
        retarget(dbToQ16(s_appliedAttDb), cfg::kUnmuteRampMs);
        muteDac(false);
        s_state = LinePathState::Running;
        ++s_starts;
        s_lastStartMs = millis() - s_startReqMs;
        LINE_LOG("[LINE] running (att %d dB, start took %lu ms)\n", static_cast<int>(s_appliedAttDb),
                 static_cast<unsigned long>(s_lastStartMs));
    } else if (s_state == LinePathState::Running && s_appliedAttDb != s_attDb) {
        s_appliedAttDb = s_attDb;
        retarget(dbToQ16(s_appliedAttDb), cfg::kUnmuteRampMs);
    }

    const int64_t t1 = esp_timer_get_time();
    const size_t frames = got / (2 * sizeof(int32_t));
    processBlock(frames);

    size_t          wrote = 0;
    const size_t    bytes = frames * 2 * sizeof(int32_t);
    const esp_err_t we = i2s_channel_write(s_tx, s_buf, bytes, &wrote, cfg::kWriteTimeoutMs);
    if (we != ESP_OK || wrote != bytes) {
        ++s_writeTimeouts;
    }
    const uint32_t procUs = static_cast<uint32_t>(esp_timer_get_time() - t1);
    if (procUs > s_maxProcessUs) s_maxProcessUs = procUs;
    ++s_blocks;
    return true;
}

// ---------------------------------------------------------------------------
// Послідовності
// ---------------------------------------------------------------------------

void doStart() {
    s_startReqMs = millis();
    muteDac(true);
    s_gCur = s_gTgt = s_gStep = 0;
    s_stallLogged = false;

    // 1. Радіо: зупинити й дати бібліотеці дограти хвіст буфера (його чути не буде, але зміну
    //    тактів не можна робити під час запису в I2S0).
    AudioPlayer::stop();
    const uint32_t w0 = millis();
    while (AudioPlayer::state() != PlayerState::Idle) {
        if (millis() - w0 > cfg::kRadioStopWaitMs) {
            LINE_LOG("[LINE] start failed: radio did not reach Idle (state %s)\n",
                     AudioPlayer::stateName(AudioPlayer::state()));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::kRadioDrainMs));

    // 2. Зафіксувати 48 кГц на I2S0 (виконує задача плеєра; лише при Idle).
    if (!AudioPlayer::setFixedOutputRate(true)) {
        LINE_LOG("[LINE] start failed: player refused fixed output rate\n");
        return;
    }
    const uint32_t c0 = millis();
    while (!AudioPlayer::fixedOutputRate()) {
        if (millis() - c0 > cfg::kClockApplyWaitMs) {
            LINE_LOG("[LINE] start failed: 48 kHz not applied on I2S0\n");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelay(pdMS_TO_TICKS(20));  // такти після reconfig стабілізуються

    // 3. DOUT -> I2S1 TX (під мʼютом).
    routeDout(true);

    // 4. Канали: RX першим, TX другим. Після enable повертаємо output-enable BCLK/WS.
    esp_err_t e = i2s_channel_enable(s_rx);
    if (e != ESP_OK) {
        LINE_LOG("[LINE] start failed: RX enable: %s\n", esp_err_to_name(e));
        return;
    }
    s_rxOn = true;
    e = i2s_channel_enable(s_tx);
    if (e != ESP_OK) {
        LINE_LOG("[LINE] start failed: TX enable: %s\n", esp_err_to_name(e));
        disableChannels();
        return;
    }
    s_txOn = true;
    const bool pinsOk = restorePinOutputs();
    LINE_LOG("[LINE] I2S1 RX+TX enabled, BCLK/WS output enable: %s\n", pinsOk ? "ok" : "MISSING");

    s_settle = cfg::kSettleBlocks;
    s_lastGoodMs = millis();
    s_state = LinePathState::Starting;
}

void doStop() {
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
    vTaskDelay(pdMS_TO_TICKS(cfg::kXsmtMuteSettleMs));
    disableChannels();
    s_state = LinePathState::Off;
    LINE_LOG("[LINE] stopped (DOUT %s)\n", s_routed ? "still on I2S1" : "on I2S0");
}

void doRadio() {
    if (s_state != LinePathState::Off) {
        doStop();
    }
    muteDac(true);
    routeDout(false);
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
    LINE_LOG("[LINE] radio path restored (XSMT stays low until the player unmutes)\n");
}

void taskEntry(void*) {
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

        const LinePathState st = s_state;
        if (st == LinePathState::Off) {
            vTaskDelay(pdMS_TO_TICKS(cfg::kIdlePollMs));
        } else {
            pump();
        }
    }
}

// ---------------------------------------------------------------------------
// Serial-тест
// ---------------------------------------------------------------------------
#if LINE_PATH_TEST
void printStats() {
    const LinePathStats s = LinePath::stats();
    Serial.printf("[LINE] state=%s muted=%d dout=%s att=%d dB\n", LinePath::stateName(s.state),
                  s.muted ? 1 : 0, s.routedToLine ? "I2S1(line)" : "I2S0(radio)",
                  static_cast<int>(s.attenuationDb));
    Serial.printf("[LINE] blocks=%lu starts=%lu stalls=%lu readTimeouts=%lu readErrors=%lu\n",
                  static_cast<unsigned long>(s.blocks), static_cast<unsigned long>(s.starts),
                  static_cast<unsigned long>(s.stalls), static_cast<unsigned long>(s.readTimeouts),
                  static_cast<unsigned long>(s.readErrors));
    Serial.printf("[LINE] writeTimeouts=%lu rxOvf=%lu txOvf=%lu maxProcess=%lu us maxReadWait=%lu us\n",
                  static_cast<unsigned long>(s.writeTimeouts),
                  static_cast<unsigned long>(s.rxOverflows),
                  static_cast<unsigned long>(s.txOverflows),
                  static_cast<unsigned long>(s.maxProcessUs),
                  static_cast<unsigned long>(s.maxReadWaitUs));
    Serial.printf("[LINE] input peak L %.1f dBFS, R %.1f dBFS (since lp.reset)\n",
                  static_cast<double>(s.peakLeftDbfs), static_cast<double>(s.peakRightDbfs));
    Serial.printf("[LINE] initMs=%lu lastStartMs=%lu stackFreeMin=%u B\n",
                  static_cast<unsigned long>(s.initMs), static_cast<unsigned long>(s.lastStartMs),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(s_task)));
    Serial.printf("[LINE] heap internal: free=%u B, largest block=%u B (begin used %d B)\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  s_heapUsedByBegin);
}

void handleLine(const char* line) {
    if (strncmp(line, "lp.", 3) != 0) {
        return;  // не наша команда (Serial можуть читати й інші тести)
    }
    const char* c = line + 3;
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
    } else {
        Serial.println("[LINE] commands: lp.start lp.stop lp.radio lp.stat lp.reset lp.att <dB>");
    }
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
#endif  // LINE_PATH_TEST

}  // namespace

// ===========================================================================
// Публічний інтерфейс
// ===========================================================================

bool LinePath::begin() {
    if (s_ready) {
        return true;
    }
    const uint32_t t0 = millis();
    const size_t heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(cfg::kI2sPort),
                                                      I2S_ROLE_SLAVE);
    cc.dma_desc_num = cfg::kDmaDescCount;
    cc.dma_frame_num = cfg::kDmaFrameCount;
    cc.auto_clear = true;  // без даних TX шле нулі (захист від «повної гучності» / повтору буфера)
    esp_err_t e = i2s_new_channel(&cc, &s_tx, &s_rx);  // duplex на одному порту
    if (e != ESP_OK) {
        Serial.printf("[LINE] channel create failed: %s\n", esp_err_to_name(e));
        s_tx = nullptr;
        s_rx = nullptr;
        return false;
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
        Serial.printf("[LINE] init failed: %s\n", esp_err_to_name(e));
        i2s_del_channel(s_rx);
        i2s_del_channel(s_tx);
        s_rx = nullptr;
        s_tx = nullptr;
        return false;
    }

    i2s_event_callbacks_t rxCb = {};
    rxCb.on_recv_q_ovf = onRxOvf;
    i2s_event_callbacks_t txCb = {};
    txCb.on_send_q_ovf = onTxOvf;
    i2s_channel_register_event_callback(s_rx, &rxCb, nullptr);
    i2s_channel_register_event_callback(s_tx, &txCb, nullptr);

    const bool pinsOk = restorePinOutputs();
    LINE_LOG("[LINE] I2S1 duplex slave ready, BCLK/WS output enable: %s (DOUT not touched)\n",
             pinsOk ? "ok" : "MISSING");

    const BaseType_t ok = xTaskCreatePinnedToCore(taskEntry, "line_path", cfg::kTaskStackBytes, nullptr,
                                                  cfg::kTaskPriority, &s_task, cfg::kTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        i2s_del_channel(s_rx);
        i2s_del_channel(s_tx);
        s_rx = nullptr;
        s_tx = nullptr;
        Serial.println("[LINE] task create failed");
        return false;
    }

#if LINE_PATH_TEST
    TaskHandle_t testHandle = nullptr;
    if (xTaskCreatePinnedToCore(testTask, "line_test", cfg::kTestTaskStackBytes, nullptr,
                                cfg::kTestTaskPriority, &testHandle, cfg::kTestTaskCore) == pdPASS) {
        Serial.println("[LINE] test mode ready (send 'lp.help')");
    } else {
        Serial.println("[LINE] test task create failed");
    }
#endif

    s_initMs = millis() - t0;
    s_heapUsedByBegin = static_cast<int>(heapBefore) -
                        static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    LINE_LOG("[LINE] begin used %d B internal RAM; free=%u B, largest block=%u B\n", s_heapUsedByBegin,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    s_ready = true;
    return true;
}

bool LinePath::isReady() {
    return s_ready;
}

bool LinePath::start() {
    if (!s_ready || s_req != kReqNone || s_state != LinePathState::Off) {
        return false;
    }
    s_req = kReqStart;
    return true;
}

bool LinePath::stop() {
    if (!s_ready || s_req != kReqNone) {
        return false;
    }
    s_req = kReqStop;
    return true;
}

bool LinePath::returnToRadio() {
    if (!s_ready || s_req != kReqNone) {
        return false;
    }
    s_req = kReqRadio;
    return true;
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
    return s;
}

void LinePath::resetStats() {
    s_blocks = s_readTimeouts = s_readErrors = s_writeTimeouts = 0;
    s_stalls = s_starts = s_maxProcessUs = s_maxReadWaitUs = 0;
    s_rxOvf = 0;
    s_txOvf = 0;
    s_peakL = 0;
    s_peakR = 0;
}

#else  // LINE_PATH_ENABLE

bool LinePath::begin() { return false; }
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
