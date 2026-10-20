#pragma once

// Константи модуля audio/: шкала й крива ЦИФРОВОЇ гучності (радіо і прохід зовнішніх входів).
// [Prompt 48a] Чип-залежні частини ВИРІЗАНО разом з TDA7318 / PT2313L: I2C (таймаути, повтори, мʼютекс),
// прапорець AUDIO_PROC_TEST і тестовий режим, kTestProcType, шкали тембру / балансу в кроках чипа, кількість
// входів чіпів, chipInputFor() / logicalInputCount(), kInputGainSteps, kPt2313lHasToneLoudness. Лишилось
// те, що використовують радіо та line_path: шкала UI 0..100, крива гучності, kRadioMaxVolumeDb, kVolStepDb,
// radioVolumeDb(). Діапазон балансу — config/balance_config.h (balance_cfg). Піни — лише config/pins.h.
//
// [Prompt 47a] ДОДАНО: kRadioMaxVolumeDb, kVolStepDb, radioVolumeDb() — крива ЦИФРОВОЇ гучності радіо.

#include <stddef.h>
#include <stdint.h>

#include "config/defaults.h"

namespace audio_cfg {

// ---------------------------------------------------------------------------
// Шкала UI
// ---------------------------------------------------------------------------
constexpr int8_t kVolumeUiMin = 0;
constexpr int8_t kVolumeUiMax = 100;

// ---------------------------------------------------------------------------
// Крива гучності: UI (0..100) -> атенюація у кроках по 1.25 дБ
// ---------------------------------------------------------------------------
// Колишній регістр гучності чіпів: 0..63 кроки по 1.25 дБ (0 = 0 дБ, 63 = -78.75 дБ). Крива збережена
// без змін (на слух та сама, що була до P47a): кусково-лінійна між якорями (значення кроків, не дБ):
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
// прив'язка верху шкали. Максимум UI = kRadioMaxVolumeDb (0 дБ: цифра нічого не підсилює). Лише
// послаблення: значення > 0 заборонено. Щоб зробити радіо тихішим чи гучнішим за все (підгонка під
// підсилювач), міняти ЛИШЕ kRadioMaxVolumeDb (≤ 0; нижчі кроки зсуваються разом із ним) або діапазон
// рівня станцій (station_level_config.h). Зовнішні входи беруть ту саму криву + kExtLevelTrimDb
// (app_controller_config.h).
constexpr float kRadioMaxVolumeDb = 0.0f;
constexpr float kVolStepDb = 1.25f;   // крок кривої гучності (колишній крок атенюатора чипів)

constexpr float radioVolumeDb(int ui) {
    return kRadioMaxVolumeDb - static_cast<float>(volumeAttSteps(ui)) * kVolStepDb;
}
static_assert(kRadioMaxVolumeDb <= 0.0f, "radio digital volume must not boost (attenuation only)");
static_assert(radioVolumeDb(kVolumeUiMax) == kRadioMaxVolumeDb, "UI max must map to kRadioMaxVolumeDb");
static_assert(radioVolumeDb(kVolumeUiMin) == kRadioMaxVolumeDb - 78.75f,
              "UI min = 63 steps * 1.25 dB below the top");

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kVolumeUiMin == 0 && kVolumeUiMax == 100, "UI volume scale is 0..100");
static_assert(kVolMaxAttSteps == 63, "volume curve is laid out for 0..63 steps");
static_assert(volCurveValid(), "volume curve anchors must be monotonic and span 0..100");
static_assert(volumeAttSteps(kVolumeUiMax) == 0, "UI max must be 0 dB");
static_assert(volumeAttSteps(kVolumeUiMin) == kVolMaxAttSteps, "UI min must be max attenuation");
static_assert(volumeAttSteps(defaults::kDefaultVolume) == 22, "default volume 40 -> 22 steps (-27.5 dB)");

}  // namespace audio_cfg
