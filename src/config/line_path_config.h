#pragma once

// Константи модуля audio/line_path (Prompt 44, прототип; P47c — kVolMaxDb = 0 дБ; P44b — життєвий цикл ресурсів;
// P47b — API гучності, мʼюту користувача і помилок). Прохід зовнішнього звуку PCM1808 (I2S1 RX, slave)
// -> ESP32-S3 -> PCM5102 (I2S1 TX, slave) на тактах BCLK GPIO15 / WS GPIO17, які веде I2S0
// (ESP32-audioI2S). DOUT GPIO18 мультиплексується між I2S0 (радіо) і I2S1 TX (прохід).
// Піни — лише з config/pins.h. Усі часові значення — у мілісекундах, якщо не вказано інше.

#include <stdint.h>

#include "config/pins.h"

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// 0 = модуль не збирається (VU входів через adc_vu, як раніше).
//     При LINE_PATH_TEST == 1 лишається Serial-тест лише з командою lp.heap.
// 1 = I2S1 належить line_path; AdcVu НЕ стартує.
#define LINE_PATH_ENABLE 1

// 1 = Serial-команди lp.* (працює і при LINE_PATH_ENABLE == 0, тоді доступна лише lp.heap).
#define LINE_PATH_TEST 0  // [P47c] 0 у виробничій збірці: lp.* керують проходом повз AppController

// Serial-лог "[LINE] ...".
#define LINE_PATH_DEBUG 1

namespace line_path_cfg {

// ---------------------------------------------------------------------------
// Формат і I2S1 (duplex slave)
// ---------------------------------------------------------------------------
// Фіксована частота, яку примусово тримає I2S0 під час проходу (Audio::setOutputSampleRate).
constexpr uint32_t kFsHz           = 48000;
constexpr int      kI2sPort        = 1;      // I2S_NUM_1 (I2S0 належить бібліотеці)
constexpr uint32_t kBlockFrames    = 128;    // стереокадрів за ітерацію (≈2.7 мс на 48 кГц)
// DMA 6 x 128: канали живуть лише під час проходу, а під радіо ці ≈12 КБ звільнені.
constexpr uint32_t kDmaDescCount   = 6;
constexpr uint32_t kDmaFrameCount  = 128;

// ---------------------------------------------------------------------------
// Задача FreeRTOS
// ---------------------------------------------------------------------------
// Ядро 1, пріоритет 6 (вище плеєра = 5 і введення = 4).
// Задача, канали I2S1 і буфер блоку створюються в start() / requestStart() і знищуються в returnToRadio().
constexpr int      kTaskCore       = 1;
constexpr uint8_t  kTaskPriority   = 6;
constexpr uint32_t kTaskStackBytes = 5120;   // без виміру НЕ зменшувати (вільний мінімум ≈3,2 КБ у P44)
constexpr uint32_t kIdlePollMs     = 20;     // сон задачі, поки прохід вимкнено
// Скільки start() (блокуючий) чекає створення каналів I2S1 задачею. Типово 10–20 мс за коментарем
// коду (окремо не вимірювалось); це верхня межа, а не очікуваний час.
constexpr uint32_t kCreateWaitMs   = 500;

// ---------------------------------------------------------------------------
// Гучність і мʼют (P47b)
// ---------------------------------------------------------------------------
// Діапазон цифрової гучності проходу, дБ (тільки послаблення; цифра не підсилює).
// Верх −6 дБ — жорсткий максимум до перевірки підсилювача на повну шкалу PCM5102.
// Нижня межа −120 дБ = dsp_volume_cfg::kMinDb (перевіряється в line_path.cpp).
constexpr float    kVolMinDb       = -120.0f;
// [P47c] Верх піднято з −6 до 0 дБ: зовнішні входи мають ту саму шкалу гучності, що й радіо (максимум UI = 0 дБ).
constexpr float    kVolMaxDb       = 0.0f;
// Гучність, яка діє, якщо її ще ніколи не задавали (безпечно тихо).
constexpr float    kStartVolumeDb  = -60.0f;
// Нижче цього значення lp.vol друкує попередження (поза звичним тестовим діапазоном).
constexpr float    kWarnVolDb      = -60.0f;

// Діапазон setAttenuationDb(int8_t) / lp.att (обгортка над setVolumeDb, зберігається для сумісності).
// [P47c] Тестовий запобіжник lp.att лишається −6 дБ (kVolMaxDb більше йому не дорівнює).
constexpr int8_t   kAttMinDb       = -60;
constexpr int8_t   kAttMaxDb       = -6;

// Рівні XSMT (PCM5102, active-low). Мають збігатись із player_cfg (static_assert у line_path.cpp).
constexpr uint8_t  kXsmtMutedLevel   = 0;
constexpr uint8_t  kXsmtUnmutedLevel = 1;

// ---------------------------------------------------------------------------
// Старт / зупинка / аварії
// ---------------------------------------------------------------------------
constexpr uint32_t kRadioStopWaitMs   = 2000; // скільки чекати PlayerState::Idle після AudioPlayer::stop()
constexpr uint32_t kRadioDrainMs      = 600;  // дограти хвіст буфера радіо (≈0.34 с) до зміни тактів
constexpr uint32_t kClockApplyWaitMs  = 1000; // скільки чекати застосування 48 кГц на I2S0
constexpr uint32_t kSettleBlocks      = 6;    // перші блоки після enable/відновлення відкидаються (у TX нулі)
// Вікно після XSMT low, протягом якого RX вичитується, а в TX пишуться нулі.
constexpr uint32_t kXsmtMuteSettleMs  = 20;
constexpr uint32_t kReadTimeoutMs     = 40;   // нема тактів / даних -> таймаут, не зависання
constexpr uint32_t kWriteTimeoutMs    = 40;
constexpr uint32_t kStallMs           = 300;  // стільки без блоків = аварійний мʼют (XSMT low, мʼют DSP)

// ---------------------------------------------------------------------------
// Serial-тест
// ---------------------------------------------------------------------------
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;
constexpr uint32_t kTestTaskStackBytes = 4096;
constexpr uint32_t kTestPollMs         = 20;
constexpr uint32_t kTestLineMax        = 64;

// lp.fault [мс]: зупинка споживання RX (перевірка Stalled -> мʼют -> відновлення).
constexpr uint32_t kFaultDefaultMs     = 800;   // має бути > kStallMs
constexpr uint32_t kFaultMaxMs         = 5000;
constexpr uint32_t kFaultRecoverWaitMs = 3000;

// lp.cycle <N>: N циклів start -> пауза -> returnToRadio із замірами RAM.
constexpr uint32_t kCycleMaxCount       = 100;
constexpr uint32_t kCycleRunMs          = 1500;
constexpr uint32_t kCycleStartWaitMs    = 4000;
constexpr uint32_t kCycleRadioWaitMs    = 4000;
constexpr uint32_t kCycleFreeSettleMs   = 300;

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
static_assert(kVolMinDb < kVolMaxDb && kVolMaxDb <= 0.0f, "volume range inverted or above 0 dB");
static_assert(kStartVolumeDb >= kVolMinDb && kStartVolumeDb <= kVolMaxDb,
              "start volume outside safe range");
static_assert(kWarnVolDb >= kVolMinDb && kWarnVolDb <= kVolMaxDb, "warn threshold outside range");
static_assert(kAttMinDb < kAttMaxDb && kAttMaxDb <= 0, "attenuation range inverted");
static_assert(static_cast<float>(kAttMaxDb) <= kVolMaxDb, "lp.att ceiling must not exceed the volume ceiling");
static_assert(static_cast<float>(kAttMinDb) == kWarnVolDb, "lp.att floor must equal warn threshold");
static_assert(kStallMs > kReadTimeoutMs, "stall must exceed one read timeout");
static_assert(kXsmtMutedLevel != kXsmtUnmutedLevel, "XSMT levels must differ");
static_assert(kCreateWaitMs >= 100, "resource creation wait too short");
static_assert(kFaultDefaultMs > kStallMs, "default fault must outlast kStallMs, or Stalled is never reached");
static_assert(kFaultMaxMs >= kFaultDefaultMs, "fault max below default");
static_assert(kCycleMaxCount >= 1, "cycle count limit");
static_assert(pins::kI2sBclk < 32 && pins::kI2sWs < 32 && pins::kI2sDout < 32,
              "pin direction fix uses GPIO.enable (GPIO < 32)");

}  // namespace line_path_cfg