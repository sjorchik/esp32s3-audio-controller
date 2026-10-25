#pragma once

// Константи діапазону балансу (індивідуального для кожного входу).
// [Prompt 48a] Створено: діапазон ±20 дБ, крок 1 дБ. Баланс = ОСЛАБЛЕННЯ ПРОТИЛЕЖНОГО КАНАЛУ в цифровому
// тракті (радіо — хук audio_process_i2s, зовнішні входи — line_path): 0 = обидва канали без змін,
// додатне значення = вправо (лівий канал послаблюється), від'ємне = вліво. У P48a поле лише
// зберігається в профілі входу (Settings::profiles[i].balanceDb) і валідується; сам DSP, енкодер, IR
// і веб-команда — P48b.
// [Prompt 48b] ДОДАНО: kSwapChannels, kRampMs. DSP балансу — audio/dsp_volume (setBalanceDb), обгортки
// radio_volume::setBalanceDb() (радіо) і LinePath::setBalanceDb() (зовнішні входи); керування — AppController.

#include <stdint.h>

namespace balance_cfg {

constexpr int8_t kMinDb     = -20;
constexpr int8_t kMaxDb     = 20;
constexpr int8_t kStepDb    = 1;
constexpr int8_t kDefaultDb = 0;

// [Prompt 48b] true: логічні канали L/R у буфері міняються місцями (на випадок, коли порядок каналів
// PCM1808 / інтерлівінгу на залізі протилежний). Конвенція не змінюється: balanceDb > 0 = «вправо».
constexpr bool kSwapChannels = false;

// [Prompt 48b] Тривалість лінійного переходу множників каналів при зміні балансу, мс (посемпловий,
// без клацань). Короткий, щоб енкодер не «відставав»; довший за один блок (≈ 2.7 мс при 128 кадрах).
constexpr uint32_t kRampMs = 30;

// true, якщо значення balanceDb допустиме (у межах діапазону). Крок перевіряє лише UI / веб.
constexpr bool isValidDb(int v) {
    return v >= kMinDb && v <= kMaxDb;
}

// Значення з NVS / запиту -> допустиме: поза діапазоном -> kDefaultDb (0).
constexpr int8_t sanitizeDb(int v) {
    return isValidDb(v) ? static_cast<int8_t>(v) : kDefaultDb;
}

static_assert(kMinDb == -kMaxDb, "balance range must be symmetric");
static_assert(kRampMs >= 1, "balance ramp must be positive");
static_assert(kStepDb >= 1, "balance step must be >= 1 dB");
static_assert(kDefaultDb >= kMinDb && kDefaultDb <= kMaxDb, "default balance must be in range");
static_assert(kDefaultDb == 0, "default balance is the neutral position (0 dB on both channels)");
static_assert(sanitizeDb(21) == kDefaultDb && sanitizeDb(-21) == kDefaultDb && sanitizeDb(-20) == -20 &&
                  sanitizeDb(20) == 20,
              "balance validation: out of range -> 0");

}  // namespace balance_cfg
