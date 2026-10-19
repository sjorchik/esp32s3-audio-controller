#pragma once

// Константи модуля audio/ (драйвери TDA7318 і PT2313L, тестовий режим).
// Піни тут не дублюються: вони лише в config/pins.h (pins::kI2cSda / kI2cScl).
// Адреса й швидкість I2C — у config/defaults.h (kAudioProcessorI2cAddr, kI2cClockHz).
//
// [Prompt 47a] ДОДАНО: kRadioMaxVolumeDb, kVolStepDb, radioVolumeDb() — крива ЦИФРОВОЇ гучності радіо.

#include <stddef.h>
#include <stdint.h>

#include "audio/audio_processor.h"
#include "config/defaults.h"

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Тестовий режим: Serial-команди для ручної перевірки процесора
// (audio/audio_proc_test.*). Вимкнути (0), коли зʼявиться AppController.
#define AUDIO_PROC_TEST 0

namespace audio_cfg {

// ---------------------------------------------------------------------------
// Вибір чипа на етапі тестування (поки немає NVS/Settings)
// ---------------------------------------------------------------------------
constexpr AudioProcType kTestProcType = AudioProcType::Pt2313l;

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------
// Таймаут однієї транзакції Wire (Wire.setTimeOut), мс.
constexpr uint16_t kI2cTimeoutMs = 50;

// Скільки ДОДАТКОВИХ спроб після невдалої транзакції (0 = без повторів).
// Повтор іде негайно, без паузи: пауза вимагала б delay() у драйвері.
constexpr uint8_t  kI2cRetries = 2;

// Скільки чекати на мʼютекс шини, мс (транзакція ≈ 0.3..1.2 мс при 100 кГц).
constexpr uint32_t kI2cMutexTimeoutMs = 100;

// Мінімальний інтервал між повідомленнями про помилку I2C в Serial, мс
// (щоб мертва шина не заливала лог під час покрокового ramp).
constexpr uint32_t kI2cErrLogMinIntervalMs = 1000;

// Пауза після подачі живлення перед першою командою, мс. Береться від millis()
// (час від старту), тому зазвичай у begin() очікування вже нульове.
// Джерело: примітка PT2313L про Cref = 10 мкФ (≥ 300 мс) — ЗВІРИТИ з даташитом;
// для TDA7318 вимоги немає, значення взято з запасом для обох.
constexpr uint32_t kPowerOnSettleMs = 300;

// ---------------------------------------------------------------------------
// Шкала UI (спільна для обох чипів)
// ---------------------------------------------------------------------------
constexpr int8_t kVolumeUiMin = 0;
constexpr int8_t kVolumeUiMax = 100;

// Тембр: кроки по kToneStepDb; ±7 кроків = ±14 дБ.
constexpr int8_t kToneUiMin   = -7;
constexpr int8_t kToneUiMax   = 7;
constexpr int8_t kToneStepDb  = 2;

// Баланс: 1 крок = 1 крок атенюатора = 1.25 дБ на «протилежному» каналі.
// +N = правий канал гучніший (лівий послаблюється на N кроків); -N навпаки.
constexpr int8_t kBalanceUiMin = -20;
constexpr int8_t kBalanceUiMax = 20;

// ---------------------------------------------------------------------------
// Крива гучності: UI (0..100) -> атенюація у кроках по 1.25 дБ
// ---------------------------------------------------------------------------
// Регістр гучності обох чіпів: 0..63 кроки по 1.25 дБ (0 = 0 дБ, 63 = -78.75 дБ),
// і сам байт даних дорівнює кількості кроків (0b00BBBAAA = 8*B + A).
// Крива — кусково-лінійна між якорями (значення кроків, не дБ):
//   UI 100 ->  0 кроків (  0.0 дБ)     UI 50 -> 16 кроків (-20 дБ)
//   UI  75 ->  8 кроків (-10.0 дБ)     UI 25 -> 32 кроки  (-40 дБ)
//   UI   0 -> 63 кроки  (-78.75 дБ)
// Значення підбирається на слух; змінювати лише таблицю якорів.
constexpr uint8_t kVolMaxAttSteps = 63;
constexpr uint8_t kVolCurveLen    = 5;
constexpr uint8_t kVolCurveUi[kVolCurveLen]    = {0, 25, 50, 75, 100};
constexpr uint8_t kVolCurveSteps[kVolCurveLen] = {63, 32, 16, 8, 0};

// Кроки атенюації (0..63) для UI-гучності. Значення поза межами обрізаються.
constexpr uint8_t volumeAttSteps(int ui) {
    if (ui <= kVolCurveUi[0]) {
        return kVolCurveSteps[0];
    }
    if (ui >= kVolCurveUi[kVolCurveLen - 1]) {
        return kVolCurveSteps[kVolCurveLen - 1];
    }
    uint8_t i = 1;
    while (ui > kVolCurveUi[i]) {
        ++i;
    }
    const int u0 = kVolCurveUi[i - 1];
    const int u1 = kVolCurveUi[i];
    const int s0 = kVolCurveSteps[i - 1];
    const int s1 = kVolCurveSteps[i];
    const int num = (s1 - s0) * (ui - u0);   // ≤ 0: атенюація спадає зі зростанням UI
    const int den = u1 - u0;
    // Округлення до найближчого цілого без float.
    const int delta = (num >= 0) ? (2 * num + den) / (2 * den)
                                 : -((-2 * num + den) / (2 * den));
    return static_cast<uint8_t>(s0 + delta);
}

constexpr bool volCurveValid() {
    if (kVolCurveUi[0] != kVolumeUiMin || kVolCurveUi[kVolCurveLen - 1] != kVolumeUiMax) {
        return false;
    }
    for (uint8_t i = 1; i < kVolCurveLen; ++i) {
        if (kVolCurveUi[i] <= kVolCurveUi[i - 1]) return false;       // UI зростає
        if (kVolCurveSteps[i] > kVolCurveSteps[i - 1]) return false;  // атенюація не зростає
    }
    return kVolCurveSteps[0] <= kVolMaxAttSteps;
}

// ---------------------------------------------------------------------------
// [Prompt 47a] ЦИФРОВА гучність радіо (audio/radio_volume): UI (0..100) -> послаблення, дБ
// ---------------------------------------------------------------------------
// Форма кривої ТА САМА, що була в аналоговій гілці (volumeAttSteps, крок 1.25 дБ); змінюється лише
// прив'язка верху шкали. Максимум UI = kRadioMaxVolumeDb (0 дБ: цифра нічого не підсилює, стара гілка
// теж давала 0 дБ на максимумі, бо gain входу = 0). Лише послаблення: значення > 0 заборонено.
// Щоб зробити радіо тихішим чи гучнішим за все (підгонка під підсилювач), міняти ЛИШЕ kRadioMaxVolumeDb
// (≤ 0; нижчі кроки зсуваються разом із ним) або діапазон рівня станцій (station_level_config.h).
constexpr float kRadioMaxVolumeDb = 0.0f;
constexpr float kVolStepDb = 1.25f;   // крок атенюатора чипів (обидва драйвери)

constexpr float radioVolumeDb(int ui) {
    return kRadioMaxVolumeDb - static_cast<float>(volumeAttSteps(ui)) * kVolStepDb;
}
static_assert(kRadioMaxVolumeDb <= 0.0f, "radio digital volume must not boost (attenuation only)");
static_assert(radioVolumeDb(kVolumeUiMax) == kRadioMaxVolumeDb, "UI max must map to kRadioMaxVolumeDb");
static_assert(radioVolumeDb(kVolumeUiMin) == kRadioMaxVolumeDb - 78.75f,
              "UI min = 63 steps * 1.25 dB below the top");

// ---------------------------------------------------------------------------
// Входи
// ---------------------------------------------------------------------------
// Фізичні стерео-входи чипів (за даташитами):
//   TDA7318 — 4 (Stereo 1..4);
//   PT2313L — 3 (Stereo 1..3; код Stereo 4 у чипі не виведений назовні).
// [Prompt 46b] Вхід ЧІПА 0..N-1 = Stereo (N+1). Логічні входи проєкту (defaults::kInputCount = 5):
// 0 = радіо, 1..4 = зовнішні; логічний k -> вхід чіпа k - 1 (chipInputFor()), тож PT2313L (3 входи)
// дає логічні 1..3, а логічний 4 для нього недоступний.
constexpr uint8_t kTda7318InputCount = 4;
constexpr uint8_t kPt2313lInputCount = 3;

// [Prompt 46b] Відображення логічних входів на входи чіпа-селектора.
//   логічний 0 (радіо)      -> вхід чіпа 0 (ЛЕГАСІ: у старому залізі радіо ще йде через вхід 0 чіпа;
//                              ПРИБРАТИ в P47 — після перепаювання радіо цифрове, селектор не потрібен);
//   логічний k = 1..4       -> вхід чіпа k - 1.
constexpr uint8_t chipInputFor(uint8_t logical) {
    return logical == 0 ? 0 : static_cast<uint8_t>(logical - 1);
}
// Кількість ЛОГІЧНИХ входів для чіпа з chipInputs фізичними входами: радіо + зовнішні (не більше kInputCount).
// TDA7318 (4) -> 5, PT2313L (3) -> 4.
constexpr uint8_t logicalInputCount(uint8_t chipInputs) {
    return static_cast<uint8_t>((chipInputs + 1u) < defaults::kInputCount ? (chipInputs + 1u)
                                                                           : defaults::kInputCount);
}
static_assert(logicalInputCount(kTda7318InputCount) == 5 && logicalInputCount(kPt2313lInputCount) == 4,
              "TDA7318: radio + 4 external; PT2313L: radio + 3 external");
static_assert(chipInputFor(0) == 0 && chipInputFor(1) == 0 && chipInputFor(4) == 3, "logical k -> chip k-1");

// Підсилення входу в КРОКАХ над 0 дБ (0..3), по одному значенню на логічний вхід.
//   TDA7318: крок 6.25 дБ (0, +6.25, +12.5, +18.75)
//   PT2313L: крок 3.75 дБ (0, +3.75, +7.5,  +11.25)
constexpr uint8_t kInputGainSteps[defaults::kInputCount] = {0, 0, 0, 0, 0};

// ---------------------------------------------------------------------------
// Варіант корпусу PT2313L
// ---------------------------------------------------------------------------
// 28-pin (DIP/SO) має тембр і loudness; 20-pin (SSOP) — не має.
// Автовизначення неможливе. Для 20-pin поставити false.
constexpr bool kPt2313lHasToneLoudness = true;

// ---------------------------------------------------------------------------
// Тестовий режим (AUDIO_PROC_TEST)
// ---------------------------------------------------------------------------
// Ядро 0, пріоритет 1 (нижче введення = 4, AppController ≈ 3, UI ≈ 2).
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;
constexpr uint32_t kTestTaskStackBytes = 4096;

// Період опитування Serial, мс.
constexpr uint32_t kTestPollMs         = 20;

// Максимальна довжина рядка команди разом із '\0'.
constexpr size_t   kTestLineMax        = 24;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kVolumeUiMin == 0 && kVolumeUiMax == 100, "UI volume scale is 0..100");
static_assert(kVolMaxAttSteps == 63, "volume register is 6 bits: 0..63");
static_assert(volCurveValid(), "volume curve anchors must be monotonic and span 0..100");
static_assert(volumeAttSteps(kVolumeUiMax) == 0, "UI max must be 0 dB");
static_assert(volumeAttSteps(kVolumeUiMin) == kVolMaxAttSteps, "UI min must be max attenuation");
static_assert(volumeAttSteps(defaults::kDefaultVolume) == 22, "default volume 40 -> 22 steps (-27.5 dB)");
static_assert(kToneUiMin == -kToneUiMax && kToneUiMax <= 7, "tone: symmetric, 4-bit code allows +-7 steps");
static_assert(kBalanceUiMin == -kBalanceUiMax, "balance range must be symmetric");
static_assert(kBalanceUiMax <= 30, "speaker attenuator: 30 steps max before mute code");
static_assert(kTda7318InputCount <= defaults::kInputCount, "more chip inputs than logical inputs");
static_assert(kPt2313lInputCount <= defaults::kInputCount, "more chip inputs than logical inputs");
static_assert(kInputGainSteps[0] <= 3 && kInputGainSteps[1] <= 3 &&
              kInputGainSteps[2] <= 3 && kInputGainSteps[3] <= 3 && kInputGainSteps[4] <= 3,
              "input gain is a 2-bit field: 0..3 steps");
static_assert(kI2cTimeoutMs > 0, "kI2cTimeoutMs must be > 0");
static_assert(kTestLineMax >= 8, "kTestLineMax too small");

}  // namespace audio_cfg