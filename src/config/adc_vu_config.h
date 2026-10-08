#pragma once

// Константи модуля audio/adc_vu (VU-метр зовнішніх входів 1..3 через АЦП PCM1808, Prompt 37).
// Піни — лише з config/pins.h: MCLK = kI2sMclk (16), DIN = kI2sDin (8); BCLK (15) і LRCLK (17)
// СПІЛЬНІ з PCM5102 (їх веде I2S0 бібліотеки ESP32-audioI2S, PCM1808 працює як slave).
// Стрепи PCM1808 (рішення власника): MD0 = MD1 = FMT = GND -> slave, авто 256/384/512 fs,
// формат I2S (Philips), 24 біти.

#include <stdint.h>

#include "config/app_controller_config.h"  // app_controller_cfg::kTaskPriority
#include "config/pins.h"

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// 1 = AdcVu увімкнено (потребує ENABLE_VU). 0 = модуль не збирається, MCLK не виводиться,
// екран ExternalInput без VU. (ENABLE_PCM1808 з features.h навмисно не використано:
// features.h не змінювався.)
#define ADC_VU_ENABLE 1

// Serial-лог "[ADCVU] ...": старт/зупинка захоплення, помилки, пік L/R раз на kLogPeriodMs.
#define ADC_VU_DEBUG 1

namespace adc_vu_cfg {

// ---------------------------------------------------------------------------
// I2S RX (slave) на ВІЛЬНОМУ порту; порт 0 належить ESP32-audioI2S (вихід на PCM5102).
// ---------------------------------------------------------------------------
constexpr int      kI2sPort        = 1;      // I2S_NUM_1
constexpr uint32_t kDmaDescNum     = 6;
constexpr uint32_t kDmaFrameNum    = 256;    // стереокадрів у одному DMA-буфері
constexpr uint32_t kBlockFrames    = 512;    // кадрів за одне читання (~11 мс на 44.1 кГц)
constexpr uint32_t kReadTimeoutMs  = 60;     // нема тактів / нема даних -> таймаут, не зависання

// ---------------------------------------------------------------------------
// Задача FreeRTOS (ядро 1 належить аудіо, тому ядро 0)
// ---------------------------------------------------------------------------
constexpr int      kTaskCore       = 0;
constexpr uint8_t  kTaskPriority   = 2;      // нижче AppController (3), як UI
constexpr uint32_t kTaskStackBytes = 4096;
constexpr uint32_t kStatePollMs    = 100;    // як часто звіряти режим/вхід з AppState
constexpr uint32_t kInactivePollMs = 100;    // сон, поки захоплення вимкнено

// ---------------------------------------------------------------------------
// Обробка
// ---------------------------------------------------------------------------
// Однополюсний ВЧ-фільтр: dc += kDcAlpha * (x - dc); сигнал = x - dc.
// Зріз ≈ kDcAlpha * fs / 2π ≈ 14 Гц при fs = 44.1 кГц.
constexpr float    kDcAlpha        = 0.002f;

// Калібрування шкали (дБ): додається до виміряного рівня. 0 дБ VU = повна шкала АЦП
// (з урахуванням того, що ВХІД TDA7318 + gain може перевантажувати АЦП раніше).
// Негативне значення зсуває шкалу вниз (VU показує менше), позитивне — вгору.
constexpr float    kAdcVuOffsetDb  = 20.0f;

// Перші блоки після вмикання каналу відкидаються (перезахоплення тактів АЦП).
constexpr uint32_t kSettleBlocks   = 2;

// Період друку піку в Serial (лише ADC_VU_DEBUG).
constexpr uint32_t kLogPeriodMs    = 1000;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kI2sPort == 1, "I2S0 belongs to ESP32-audioI2S; ADC capture must use I2S1");
static_assert(pins::kI2sBclk < 32 && pins::kI2sWs < 32,
              "pin direction fix uses GPIO.enable (GPIO < 32)");
static_assert(kBlockFrames >= kDmaFrameNum && kBlockFrames <= 2048, "block size out of range");
static_assert(kTaskPriority < app_controller_cfg::kTaskPriority,
              "ADC VU task must stay below AppController");
static_assert(kDcAlpha > 0.0f && kDcAlpha < 0.1f, "kDcAlpha out of range");
static_assert(kStatePollMs >= 10 && kInactivePollMs >= 10, "poll periods too short");

}  // namespace adc_vu_cfg
