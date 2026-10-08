#pragma once

// VU-метр зовнішніх входів (TV Box / Computer / Aux = входи 1..3) через АЦП PCM1808.
// [Prompt 37] Сигнал знімається з OUT(L)/OUT(R) TDA7318 (піни 17/7 мікросхеми): це вихід
// селектора входів із gain, ДО регулятора гучності й тембру, тож рівень НЕ залежить від
// гучності/тембру (лише від вибраного входу й gain).
//
// Тактування: PCM1808 — slave; MCLK (GPIO16, 256·fs), BCLK (15) і LRCLK (17) безперервно веде
// I2S0 бібліотеки ESP32-audioI2S (канал не зупиняється при stopSong(), auto_clear = true).
// Тут створюється ЛИШЕ I2S1 RX (slave) на тих самих BCLK/LRCLK + DIN (GPIO8); I2S0 не чіпається,
// окрім відновлення напрямку пінів 15/17 (див. adc_vu.cpp).
//
// Захоплення працює лише коли mode == ExternalInput, вхід 1..3 і немає перезапуску.
// Споживач VU — ui/screens: VuSourceAdc::read() (display-задача).

#include <stdint.h>

#include "audio/vu_source.h"
#include "config/adc_vu_config.h"

class AdcVu {
public:
    // Створює I2S1 RX і задачу захоплення. Викликати ПІСЛЯ AudioPlayer::begin() (там setPinout
    // I2S0) і ПІСЛЯ AppController::begin() (задача читає AppState). Повторний виклик безпечний.
    // false — помилка ініціалізації (один лог); VU тоді мовчки «без даних».
    static bool begin();

    // true, якщо begin() пройшов успішно.
    static bool isReady();
};

// Джерело VU з АЦП. read() — лише з однієї задачі (display), без блокувань.
class VuSourceAdc : public VuSource {
public:
    bool read(float& left, float& right) override;

private:
    bool     m_init = false;
    bool     m_flowing = false;
    bool     m_everFlowed = false;
    bool     m_warnedNoData = false;
    uint32_t m_lastReadMs = 0;
    uint32_t m_sinceMs = 0;
    uint32_t m_lastBlocks = 0;
    uint32_t m_lastChangeMs = 0;
    float    m_levelL = 0.0f;
    float    m_levelR = 0.0f;
};
