#pragma once

// Константи модуля audio/vu_source (VU-метр з декодованих PCM-семплів радіо).
// Геометрія й кольори смуг — у config/screens_config.h (screens_cfg::kVu*).

#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог VU ("[VU] ..."): лише в display-задачі, на змінах стану
// (потік пішов / зупинився / PCM так і не прийшов). У гарячому шляху аудіо логів немає.
#define VU_DEBUG 1

// [VU1] Режим вимірювання рівня VU (продюсер + споживач):
//   0 = ПІК за блок (поведінка до VU1: publishPeaks / peakToNorm, бітово як раніше);
//   1 = RMS: продюсер накопичує суму квадратів і кількість кадрів (publishEnergy), display-задача бере
//       середній квадрат за інтервал між двома read() і переводить у дБ (vu_math::msqToNorm).
// Перемикач для відкату й порівняння; значення 0 повертає старий шлях без жодних змін у гарячому шляху.
#define VU_MEASURE_MODE 1

// Спосіб перехоплення PCM у ESP32-audioI2S — ЗАЛЕЖИТЬ ВІД ВЕРСІЇ БІБЛІОТЕКИ (звірити!):
//   1 = вільна глобальна функція (звірено з Audio.h вашої збірки)
//       void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S)
//       (слабке оголошення в Audio.h; outBuff — int32, validSamples — слів обох каналів);
//   0 = хука немає / сигнатура інша: продюсер не компілюється, read() завжди false
//       (VU буде порожнім, але проєкт збереться).
// УВАГА: при НЕВІРНІЙ сигнатурі компілятор мовчить (це просто інша функція, яку
// бібліотека не викликає) — ознака лише в Serial: "[VU] no PCM blocks ...".
#define VU_PCM_HOOK_STYLE 1

namespace vu_cfg {

// ---------------------------------------------------------------------------
// Вимірювання (продюсер, гарячий шлях аудіо)
// ---------------------------------------------------------------------------
// Шкала пікових значень, що приходять у publishPeaks(): int16-еквівалент
// (хук зсуває int32-відліки на 16 біт), |-32768| = 32768 = 0 dBFS.
constexpr float    kFullScale = 32768.0f;

// Верхня межа стереокадрів за один виклик хука (захист від виходу за буфер).
// Реально блок = DMA_FRAME_NUM (256) кадрів; 2048 — із запасом.
constexpr int32_t  kMaxFramesPerBlock = 2048;

// [VU1] RMS: сума квадратів у шкалі int16 (повна шкала 32768 -> 2^30 на кадр). Продюсер веде 64-бітний
// накопичувач (лише в задачі продюсера) і публікує молодші 32 біти (sum >> kEnergyShift): споживач бере
// різницю за модулем 2^32, тож обгортання лічильника нешкідливе (інтервал до ≈ 5 с при повній шкалі).
constexpr uint32_t kEnergyShift  = 16;
// Верхня межа суми квадратів за ОДИН блок (одиниці int16²): захист 32-бітного лічильника від переповнення
// після калібрування й EQ-підсилення; захищає й від NaN / inf (`!(x < cap)` -> cap).
constexpr float    kEnergySumCap = 2.0e14f;
// Свіжий знімок споживач читає за seqlock; після цієї кількості невдалих спроб (продюсер саме пише) кадр
// пропускається, рівень лишається попереднім.
constexpr uint8_t  kEnergyReadRetries = 4;
// Найкоротше вікно усереднення, мс. Якщо між двома read() минуло менше, нові кадри копляться до наступного
// виклику, а рівень береться з попереднього вікна (RMS на вікні коротшому за період баса ≈ 25 мс «пульсує»).
constexpr uint32_t kRmsMinWindowMs = 20;
// Калібрування шкали: повна шкала синус 0 dBFS має RMS на 3,01 дБ нижче піка. Додається до рівня RMS у дБ
// ПЕРЕД зсувом чутливості, щоб синус 0 dBFS показувався так само, як і за піком (10*log10(2)).
// ЄДИНЕ місце налаштування калібрування RMS.
constexpr float    kRmsOffsetDb  = 3.01029996f;
// Обмеження відношення (RMS / повна шкала)² перед log10f у споживачі (захист від inf); значення багато вище 0 дБ
// на шкалі все одно обрізається нормалізацією.
constexpr float    kRmsRatioCap  = 1.0e6f;

// ---------------------------------------------------------------------------
// Шкала й балістика (споживач, display-задача)
// ---------------------------------------------------------------------------
// Рівень відображається в децибелах: 0 dBFS = 1.0, kDbFloor = 0.0. Лінійна шкала
// амплітуди для музики майже завжди була б у нижній третині смуги.
constexpr float    kDbFloor    = -21.0f;

// Експоненційне згладжування з урахуванням dt: швидка атака, повільний спад.
// alpha = dt / (tau + dt).
constexpr float    kAttackMs   = 5.0f;
constexpr float    kReleaseMs  = 80.0f;

// Якщо нових PCM-блоків немає стільки мс — потік не грає: read() повертає false,
// рівні скидаються в 0.
constexpr uint32_t kStaleMs     = 300;

// Якщо між двома read() минуло більше (екран не малював VU: пауза, інший режим) —
// згладжування й накопичений пік скидаються, щоб не показати старе значення.
constexpr uint32_t kResumeGapMs = 250;

// Скільки мс після початку опитування (Playing) чекати на перший PCM-блок, перш ніж
// друкувати "no PCM blocks": бібліотека може віддавати перші блоки в I2S із затримкою
// (попередня буферизація), тому 300 мс (kStaleMs) для попередження замало.
constexpr uint32_t kNoDataWarnMs = 8000;

// ---------------------------------------------------------------------------
// Пороги кольору (частка шкали 0..1; з kDbFloor -20 дБ: 0.70 ≈ -6 дБ, 0.90 ≈ -2 дБ)
// ---------------------------------------------------------------------------
constexpr float    kYellowFrom = 0.70f;
constexpr float    kRedFrom    = 0.90f;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kFullScale > 0.0f, "kFullScale must be positive");
static_assert(kMaxFramesPerBlock > 0, "kMaxFramesPerBlock must be positive");
static_assert(kDbFloor < 0.0f, "kDbFloor must be negative");
static_assert(VU_MEASURE_MODE == 0 || VU_MEASURE_MODE == 1, "VU_MEASURE_MODE: 0 = peak, 1 = RMS");
static_assert(kEnergyShift >= 8 && kEnergyShift <= 24, "kEnergyShift out of range");
static_assert(kEnergySumCap > 0.0f && kEnergySumCap / static_cast<float>(1ull << kEnergyShift) < 4.0e9f,
              "kEnergySumCap must fit the 32-bit published counter");
static_assert(kEnergyReadRetries >= 1, "kEnergyReadRetries must be >= 1");
static_assert(kRmsMinWindowMs < kResumeGapMs, "min RMS window must be shorter than the resume gap");
static_assert(kRmsOffsetDb > 3.0f && kRmsOffsetDb < 3.02f, "kRmsOffsetDb must be 10*log10(2)");
static_assert(kRmsRatioCap > 1.0f, "kRmsRatioCap must exceed 1");
static_assert(kAttackMs > 0.0f && kReleaseMs >= kAttackMs, "release must be >= attack > 0");
static_assert(kStaleMs > 0 && kResumeGapMs > 0 && kNoDataWarnMs > kStaleMs, "stale/resume intervals must be positive");
static_assert(kYellowFrom > 0.0f && kYellowFrom < kRedFrom && kRedFrom <= 1.0f,
              "color thresholds must satisfy 0 < yellow < red <= 1");

}  // namespace vu_cfg
