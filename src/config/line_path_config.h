#pragma once

// Константи модуля audio/line_path (Prompt 44, ПРОТОТИП): прохід зовнішнього звуку
// PCM1808 (I2S1 RX, slave) -> ESP32-S3 -> PCM5102 (I2S1 TX, slave) на тих самих тактах
// BCLK GPIO15 / WS GPIO17, які веде I2S0 (ESP32-audioI2S). DOUT GPIO18 мультиплексується
// між I2S0 (радіо) і I2S1 TX (прохід) через GPIO-матрицю.
// Піни — лише з config/pins.h. Усі часові значення — у мілісекундах, якщо не вказано інше.

#include <stdint.h>

#include "config/pins.h"

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// 0 = модуль не збирається, поведінка проєкту як раніше (VU входів через adc_vu).
// 1 = I2S1 належить line_path; AdcVu НЕ стартує (VU зовнішніх входів у прототипі недоступний).
#define LINE_PATH_ENABLE 1

// 1 = Serial-команди lp.* (потребує LINE_PATH_ENABLE). Читають той самий Serial, що й інші тести.
#define LINE_PATH_TEST 1

// Serial-лог "[LINE] ...".
#define LINE_PATH_DEBUG 1

namespace line_path_cfg {

// ---------------------------------------------------------------------------
// Формат і I2S1 (duplex slave)
// ---------------------------------------------------------------------------
// Фіксована частота, яку примусово тримає I2S0 під час проходу (Audio::setOutputSampleRate).
// Для самого проходу в slave-режимі вона не критична (RX і TX йдуть за зовнішніми тактами),
// але потрібна як відома fs для EQ / VU у наступних кроках.
constexpr uint32_t kFsHz           = 48000;
constexpr int      kI2sPort        = 1;      // I2S_NUM_1 (I2S0 належить бібліотеці)
constexpr uint32_t kBlockFrames    = 128;    // стереокадрів за ітерацію (≈2.7 мс на 48 кГц)
constexpr uint32_t kDmaDescCount   = 6;      // DMA-дескрипторів на напрямок
constexpr uint32_t kDmaFrameCount  = 128;    // кадрів в одному DMA-буфері

// ---------------------------------------------------------------------------
// Задача FreeRTOS
// ---------------------------------------------------------------------------
// Ядро 1: під час проходу радіо зупинене, а єдина інша задача ядра 1 (audio_player, пріоритет 5)
// лише крутить порожній Audio::loop(). Пріоритет 6 — вище за плеєр і введення (4).
constexpr int      kTaskCore       = 1;
constexpr uint8_t  kTaskPriority   = 6;
constexpr uint32_t kTaskStackBytes = 5120;
constexpr uint32_t kIdlePollMs     = 20;     // сон задачі, поки прохід вимкнено

// ---------------------------------------------------------------------------
// Рівень і мʼют
// ---------------------------------------------------------------------------
// Фіксоване послаблення проходу, дБ. ТІЛЬКИ послаблення: діапазон [kAttMinDb, kAttMaxDb].
constexpr int8_t   kPrototypeAttenuationDb = -20;
constexpr int8_t   kAttMinDb               = -60;
constexpr int8_t   kAttMaxDb               = -6;   // «безпечна стеля» прототипу

constexpr uint32_t kUnmuteRampMs   = 100;    // лінійне наростання підсилення 0 -> ціль
constexpr uint32_t kMuteRampMs     = 20;     // лінійний спад ціль -> 0 перед XSMT low

// Рівні XSMT (PCM5102, active-low). Мають збігатись із player_cfg (static_assert у line_path.cpp).
constexpr uint8_t  kXsmtMutedLevel   = 0;
constexpr uint8_t  kXsmtUnmutedLevel = 1;

// ---------------------------------------------------------------------------
// Старт / зупинка / аварії
// ---------------------------------------------------------------------------
constexpr uint32_t kRadioStopWaitMs   = 2000; // скільки чекати PlayerState::Idle після AudioPlayer::stop()
constexpr uint32_t kRadioDrainMs      = 600;  // дати бібліотеці дограти хвіст буфера (≈0.34 с) до зміни тактів
constexpr uint32_t kClockApplyWaitMs  = 1000; // скільки чекати застосування 48 кГц на I2S0
constexpr uint32_t kSettleBlocks      = 6;    // перші блоки після enable/відновлення тактів відкидаються
constexpr uint32_t kXsmtMuteSettleMs  = 20;   // пауза після XSMT low перед вимкненням каналів
constexpr uint32_t kReadTimeoutMs     = 40;   // нема тактів / даних -> таймаут, не зависання
constexpr uint32_t kWriteTimeoutMs    = 40;
constexpr uint32_t kStallMs           = 300;  // стільки без блоків = аварійний мʼют (XSMT low, підсилення 0)

// Serial-тест.
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;
constexpr uint32_t kTestTaskStackBytes = 4096;
constexpr uint32_t kTestPollMs         = 20;
constexpr uint32_t kTestLineMax        = 64;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kI2sPort == 1, "I2S0 belongs to ESP32-audioI2S; line path must use I2S1");
static_assert(kFsHz == 48000, "prototype is fixed at 48 kHz (Audio::SR_48000)");
static_assert(kBlockFrames >= 32 && kBlockFrames <= 1024, "block size out of range");
static_assert(kDmaFrameCount >= 32 && kDmaFrameCount <= 1024, "DMA frame count out of range");
static_assert(kDmaDescCount >= 3 && kDmaDescCount <= 16, "DMA descriptor count out of range");
static_assert(kTaskCore == 1 || kTaskCore == 0, "bad core");
static_assert(kTaskPriority >= 5, "line path task must be >= audio_player priority (5)");
static_assert(kAttMinDb < kAttMaxDb && kAttMaxDb <= 0, "attenuation range inverted");
static_assert(kPrototypeAttenuationDb >= kAttMinDb && kPrototypeAttenuationDb <= kAttMaxDb,
              "default attenuation outside safe range");
static_assert(kUnmuteRampMs >= 10 && kMuteRampMs >= 5, "ramps too short (clicks)");
static_assert(kStallMs > kReadTimeoutMs, "stall must exceed one read timeout");
static_assert(kXsmtMutedLevel != kXsmtUnmutedLevel, "XSMT levels must differ");
static_assert(pins::kI2sBclk < 32 && pins::kI2sWs < 32 && pins::kI2sDout < 32,
              "pin direction fix uses GPIO.enable (GPIO < 32)");

}  // namespace line_path_cfg
