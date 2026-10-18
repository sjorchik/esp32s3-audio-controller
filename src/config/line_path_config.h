#pragma once

// Константи модуля audio/line_path (Prompt 44, ПРОТОТИП; Prompt 44b — життєвий цикл ресурсів):
// прохід зовнішнього звуку PCM1808 (I2S1 RX, slave) -> ESP32-S3 -> PCM5102 (I2S1 TX, slave) на тих
// самих тактах BCLK GPIO15 / WS GPIO17, які веде I2S0 (ESP32-audioI2S). DOUT GPIO18 мультиплексується
// між I2S0 (радіо) і I2S1 TX (прохід) через GPIO-матрицю.
// Піни — лише з config/pins.h. Усі часові значення — у мілісекундах, якщо не вказано інше.

#include <stdint.h>

#include "config/pins.h"

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// 0 = модуль не збирається, поведінка проєкту як раніше (VU входів через adc_vu).
//     [P44b] При LINE_PATH_TEST == 1 лишається Serial-тест лише з командою lp.heap (еталон RAM без line_path).
// 1 = I2S1 належить line_path; AdcVu НЕ стартує (VU зовнішніх входів у прототипі недоступний).
#define LINE_PATH_ENABLE 1

// 1 = Serial-команди lp.* (читають той самий Serial, що й інші тести). Працює і при LINE_PATH_ENABLE == 0
// (тоді доступна лише lp.heap).
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
// [P44b] DMA лишено 6 x 128 (як у P44): канали живуть лише під час проходу, а під радіо ці ≈12 КБ
// звільнені, тож зменшення не дає виграшу, а глибина черги ≈16 мс страхує від джитера ядра 1.
constexpr uint32_t kDmaDescCount   = 6;      // DMA-дескрипторів на напрямок
constexpr uint32_t kDmaFrameCount  = 128;    // кадрів в одному DMA-буфері

// ---------------------------------------------------------------------------
// Задача FreeRTOS
// ---------------------------------------------------------------------------
// Ядро 1: під час проходу радіо зупинене, а єдина інша задача ядра 1 (audio_player, пріоритет 5)
// лише крутить порожній Audio::loop(). Пріоритет 6 — вище за плеєр і введення (4).
// [P44b] Задача, канали I2S1 і буфер блоку створюються в start() і знищуються в returnToRadio().
constexpr int      kTaskCore       = 1;
constexpr uint8_t  kTaskPriority   = 6;
constexpr uint32_t kTaskStackBytes = 5120;   // без виміру НЕ зменшувати (вільний мінімум ≈3,2 КБ у P44)
constexpr uint32_t kIdlePollMs     = 20;     // сон задачі, поки прохід вимкнено (між stop і returnToRadio)
constexpr uint32_t kCreateWaitMs   = 500;    // [P44b] скільки start() чекає створення каналів I2S1 задачею

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
constexpr uint32_t kSettleBlocks      = 6;    // перші блоки після enable/відновлення тактів відкидаються (у TX — нулі)
// [P44b] Вікно після XSMT low, протягом якого RX вичитується, а в TX пишуться нулі (канали не стоять
// увімкненими без обслуговування); потім канали вимикаються.
constexpr uint32_t kXsmtMuteSettleMs  = 20;
constexpr uint32_t kReadTimeoutMs     = 40;   // нема тактів / даних -> таймаут, не зависання
constexpr uint32_t kWriteTimeoutMs    = 40;
constexpr uint32_t kStallMs           = 300;  // стільки без блоків = аварійний мʼют (XSMT low, підсилення 0)

// ---------------------------------------------------------------------------
// Serial-тест
// ---------------------------------------------------------------------------
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;
constexpr uint32_t kTestTaskStackBytes = 4096;
constexpr uint32_t kTestPollMs         = 20;
constexpr uint32_t kTestLineMax        = 64;

// [P44b] lp.fault [мс]: штучна зупинка споживання RX-блоків (перевірка Stalled -> мʼют -> відновлення).
constexpr uint32_t kFaultDefaultMs     = 800;   // має бути > kStallMs, інакше Stalled не настане
constexpr uint32_t kFaultMaxMs         = 5000;
constexpr uint32_t kFaultRecoverWaitMs = 3000;  // скільки після кінця збою чекати повернення в Running

// [P44b] lp.cycle <N>: N циклів start -> пауза -> returnToRadio із замірами RAM.
constexpr uint32_t kCycleMaxCount       = 100;
constexpr uint32_t kCycleRunMs          = 1500; // скільки тримати прохід Running у кожному циклі
constexpr uint32_t kCycleStartWaitMs    = 4000; // скільки чекати Running після start()
constexpr uint32_t kCycleRadioWaitMs    = 4000; // скільки чекати завершення returnToRadio (задача зникла)
constexpr uint32_t kCycleFreeSettleMs   = 300;  // пауза, щоб IDLE-задача звільнила стек і TCB видаленої задачі

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
static_assert(kCreateWaitMs >= 100, "resource creation wait too short");
static_assert(kFaultDefaultMs > kStallMs, "default fault must outlast kStallMs, or Stalled is never reached");
static_assert(kFaultMaxMs >= kFaultDefaultMs, "fault max below default");
static_assert(kCycleMaxCount >= 1, "cycle count limit");
static_assert(pins::kI2sBclk < 32 && pins::kI2sWs < 32 && pins::kI2sDout < 32,
              "pin direction fix uses GPIO.enable (GPIO < 32)");

}  // namespace line_path_cfg
