// VU-метр зовнішніх входів через PCM1808 (Prompt 37). Опис — в audio/adc_vu.h.
//
// Обмін із display-задачею — як у vu_source.cpp: lock-free atomics (пік «з моменту останнього
// читання» + лічильник блоків). Єдиний письменник — задача захоплення.
//
// ВАЖЛИВО про піни 15/17: вони водночас ВИХІД I2S0 (на PCM5102) і ВХІД I2S1 RX (slave).
// Драйвер IDF при ініціалізації slave-входу може залишити пін «лише входом» і тим вимкнути вихід
// I2S0. Тому після ініціалізації/вмикання RX вмикаємо ще й output-enable НИЗЬКОРІВНЕВО
// (gpio_ll_*; gpio_set_direction() не використовуємо — він може перепідключити вихідний сигнал
// піна до GPIO-вентиля й обірвати I2S0). Сигнал виходу з матриці I2S0 не змінюється.

#include "audio/adc_vu.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include <atomic>

#include "config/adc_vu_config.h"
#include "config/features.h"
#include "config/vu_config.h"

#if ADC_VU_ENABLE && ENABLE_VU

#include "audio/vu_math.h"
#include "core/app_state.h"
#include "driver/i2s_std.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#if ADC_VU_DEBUG
#define ADCVU_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define ADCVU_LOG(...) \
    do {               \
    } while (0)
#endif

namespace {

std::atomic<uint32_t> s_peakL{0};
std::atomic<uint32_t> s_peakR{0};
std::atomic<uint32_t> s_blocks{0};

constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

i2s_chan_handle_t s_rx = nullptr;
TaskHandle_t      s_task = nullptr;
bool              s_ready = false;

// Інтерліврований L,R,L,R...; 24 біти даних вирівняні вліво в 32-бітному слові.
int32_t s_buf[adc_vu_cfg::kBlockFrames * 2];

// Повернути output-enable пінам BCLK/LRCLK (див. шапку файла) і перевірити, що він справді є.
bool restorePinOutputs() {
    gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sBclk));
    gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sWs));
    gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sBclk));
    gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sWs));
    const bool bclkOut = ((GPIO.enable >> pins::kI2sBclk) & 1u) != 0;
    const bool wsOut = ((GPIO.enable >> pins::kI2sWs) & 1u) != 0;
    return bclkOut && wsOut;
}

bool wantActive() {
    const AppStateData s = AppState::snapshot();
    return s.mode == Mode::ExternalInput && s.inputIndex != 0 && !s.restarting;
}

void captureTask(void*) {
    const float offsetLin = powf(10.0f, adc_vu_cfg::kAdcVuOffsetDb / 20.0f);
    bool     enabled = false;
    bool     want = false;
    uint32_t lastPollMs = 0;
    uint32_t settle = 0;
    bool     warnedTimeout = false;
    float    dcL = 0.0f;
    float    dcR = 0.0f;
    uint32_t logPeakL = 0;
    uint32_t logPeakR = 0;
    uint32_t lastLogMs = 0;

    for (;;) {
        const uint32_t now = millis();
        if (now - lastPollMs >= adc_vu_cfg::kStatePollMs) {
            lastPollMs = now;
            want = wantActive();
        }

        if (want != enabled) {
            if (want) {
                const esp_err_t e = i2s_channel_enable(s_rx);
                if (e == ESP_OK) {
                    enabled = true;
                    settle = adc_vu_cfg::kSettleBlocks;
                    warnedTimeout = false;
                    dcL = 0.0f;
                    dcR = 0.0f;
                    const bool pinsOk = restorePinOutputs();
                    ADCVU_LOG("[ADCVU] capture on, BCLK/WS output enable: %s\n",
                              pinsOk ? "ok" : "MISSING");
                } else {
                    ADCVU_LOG("[ADCVU] enable failed: %s\n", esp_err_to_name(e));
                    vTaskDelay(pdMS_TO_TICKS(adc_vu_cfg::kInactivePollMs));
                    continue;
                }
            } else {
                i2s_channel_disable(s_rx);
                enabled = false;
                ADCVU_LOG("[ADCVU] capture off\n");
            }
        }

        if (!enabled) {
            vTaskDelay(pdMS_TO_TICKS(adc_vu_cfg::kInactivePollMs));
            continue;
        }

        size_t          got = 0;
        const esp_err_t e = i2s_channel_read(s_rx, s_buf, sizeof(s_buf), &got,
                                             adc_vu_cfg::kReadTimeoutMs);
        if (e != ESP_OK || got < 2 * sizeof(int32_t)) {
            // Нема тактів (I2S0 перенастроюється) або АЦП відсутній: просто нічого не публікуємо,
            // display за kStaleMs покаже «без даних». Таймаут сам по собі блокує, не крутиться.
            if (!warnedTimeout) {
                warnedTimeout = true;
                ADCVU_LOG("[ADCVU] no data (%s) - check PCM1808 / clocks\n", esp_err_to_name(e));
            }
            if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            settle = adc_vu_cfg::kSettleBlocks;  // після пропуску знову відкинути перші блоки
            continue;
        }
        warnedTimeout = false;
        if (settle > 0) {
            --settle;
            continue;
        }

        const size_t frames = got / (2 * sizeof(int32_t));
        float        peakL = 0.0f;
        float        peakR = 0.0f;
        for (size_t i = 0; i < frames; ++i) {
            // 24 біти в старших бітах слова: >> 8 -> знакове 24-біт; /256 -> шкала int16 (±32768).
            const float xl = static_cast<float>(s_buf[2 * i] >> 8) * (1.0f / 256.0f);
            const float xr = static_cast<float>(s_buf[2 * i + 1] >> 8) * (1.0f / 256.0f);
            dcL += adc_vu_cfg::kDcAlpha * (xl - dcL);
            dcR += adc_vu_cfg::kDcAlpha * (xr - dcR);
            const float yl = fabsf(xl - dcL);
            const float yr = fabsf(xr - dcR);
            if (yl > peakL) peakL = yl;
            if (yr > peakR) peakR = yr;
        }

        const uint32_t pl = static_cast<uint32_t>(peakL * offsetLin);
        const uint32_t pr = static_cast<uint32_t>(peakR * offsetLin);
        if (pl > s_peakL.load(kRelaxed)) s_peakL.store(pl, kRelaxed);
        if (pr > s_peakR.load(kRelaxed)) s_peakR.store(pr, kRelaxed);
        s_blocks.store(s_blocks.load(kRelaxed) + 1u, kRelaxed);  // єдиний письменник

#if ADC_VU_DEBUG
        if (pl > logPeakL) logPeakL = pl;
        if (pr > logPeakR) logPeakR = pr;
        if (now - lastLogMs >= adc_vu_cfg::kLogPeriodMs) {
            lastLogMs = now;
            const float dbL = (logPeakL > 0) ? 20.0f * log10f(logPeakL / vu_cfg::kFullScale) : -99.0f;
            const float dbR = (logPeakR > 0) ? 20.0f * log10f(logPeakR / vu_cfg::kFullScale) : -99.0f;
            Serial.printf("[ADCVU] peak L %.1f dBFS, R %.1f dBFS\n", dbL, dbR);
            logPeakL = 0;
            logPeakR = 0;
        }
#else
        (void)logPeakL;
        (void)logPeakR;
        (void)lastLogMs;
#endif
    }
}

}  // namespace

bool AdcVu::begin() {
    if (s_ready) {
        return true;
    }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(adc_vu_cfg::kI2sPort),
                                                      I2S_ROLE_SLAVE);
    cc.dma_desc_num = adc_vu_cfg::kDmaDescNum;
    cc.dma_frame_num = adc_vu_cfg::kDmaFrameNum;
    esp_err_t e = i2s_new_channel(&cc, nullptr, &s_rx);
    if (e != ESP_OK) {
        Serial.printf("[ADCVU] channel create failed: %s (external VU disabled)\n", esp_err_to_name(e));
        s_rx = nullptr;
        return false;
    }

    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000);  // у slave частоту задає зовнішній LRCLK
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;  // MCLK веде I2S0 (на GPIO16 -> SCKI PCM1808)
    sc.gpio_cfg.bclk = static_cast<gpio_num_t>(pins::kI2sBclk);
    sc.gpio_cfg.ws = static_cast<gpio_num_t>(pins::kI2sWs);
    sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    sc.gpio_cfg.din = static_cast<gpio_num_t>(pins::kI2sDin);
    e = i2s_channel_init_std_mode(s_rx, &sc);
    if (e != ESP_OK) {
        Serial.printf("[ADCVU] init failed: %s (external VU disabled)\n", esp_err_to_name(e));
        i2s_del_channel(s_rx);
        s_rx = nullptr;
        return false;
    }

    // Піни могли стати «лише входом» — повертаємо вихід I2S0 одразу, до першого enable.
    const bool pinsOk = restorePinOutputs();
    ADCVU_LOG("[ADCVU] I2S1 RX slave ready, BCLK/WS output enable: %s\n", pinsOk ? "ok" : "MISSING");

    const BaseType_t ok = xTaskCreatePinnedToCore(captureTask, "adc_vu", adc_vu_cfg::kTaskStackBytes,
                                                  nullptr, adc_vu_cfg::kTaskPriority, &s_task,
                                                  adc_vu_cfg::kTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        i2s_del_channel(s_rx);
        s_rx = nullptr;
        Serial.println("[ADCVU] task create failed (external VU disabled)");
        return false;
    }
    s_ready = true;
    return true;
}

bool AdcVu::isReady() {
    return s_ready;
}

bool VuSourceAdc::read(float& left, float& right) {
    left = 0.0f;
    right = 0.0f;
    if (!s_ready) {
        return false;
    }
    const uint32_t now = millis();
    const uint32_t blocks = s_blocks.load(kRelaxed);

    float dtMs = 0.0f;
    if (!m_init || (now - m_lastReadMs) > vu_cfg::kResumeGapMs) {
        m_levelL = 0.0f;
        m_levelR = 0.0f;
        m_lastBlocks = blocks;
        m_lastChangeMs = now;
        m_sinceMs = now;
        m_flowing = false;
        m_init = true;
        s_peakL.exchange(0, kRelaxed);
        s_peakR.exchange(0, kRelaxed);
    } else {
        dtMs = static_cast<float>(now - m_lastReadMs);
    }
    m_lastReadMs = now;

    if (blocks != m_lastBlocks) {
        m_lastBlocks = blocks;
        m_lastChangeMs = now;
        if (!m_flowing) {
            m_flowing = true;
            m_everFlowed = true;
        }
    } else if (now - m_lastChangeMs >= vu_cfg::kStaleMs) {
        m_levelL = 0.0f;
        m_levelR = 0.0f;
        s_peakL.exchange(0, kRelaxed);
        s_peakR.exchange(0, kRelaxed);
        m_flowing = false;
        if (!m_everFlowed && !m_warnedNoData && now - m_sinceMs >= vu_cfg::kNoDataWarnMs) {
            m_warnedNoData = true;
            ADCVU_LOG("[ADCVU] no ADC blocks on external input - check PCM1808 power/clocks/wiring\n");
        }
        return false;
    }

    const uint32_t pl = s_peakL.exchange(0, kRelaxed);
    const uint32_t pr = s_peakR.exchange(0, kRelaxed);
    m_levelL = vu_math::ballistics(m_levelL, vu_math::peakToNorm(pl), dtMs);
    m_levelR = vu_math::ballistics(m_levelR, vu_math::peakToNorm(pr), dtMs);
    left = m_levelL;
    right = m_levelR;
    return true;
}

#else  // ADC_VU_ENABLE && ENABLE_VU

bool AdcVu::begin() {
    return false;
}
bool AdcVu::isReady() {
    return false;
}
bool VuSourceAdc::read(float& left, float& right) {
    left = 0.0f;
    right = 0.0f;
    return false;
}

#endif
