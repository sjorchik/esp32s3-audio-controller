#pragma once

// Константи діапазону балансу (індивідуального для кожного входу).
// [Prompt 48a] Створено: діапазон ±20 дБ, крок 1 дБ. Баланс = ОСЛАБЛЕННЯ ПРОТИЛЕЖНОГО КАНАЛУ в цифровому
// тракті (радіо — хук audio_process_i2s, зовнішні входи — line_path): 0 = обидва канали без змін,
// додатне значення = вправо (лівий канал послаблюється), від'ємне = вліво. У P48a поле лише
// зберігається в профілі входу (Settings::profiles[i].balanceDb) і валідується; сам DSP, енкодер, IR
// і веб-команда — P48b.

#include <stdint.h>

namespace balance_cfg {

constexpr int8_t kMinDb     = -20;
constexpr int8_t kMaxDb     = 20;
constexpr int8_t kStepDb    = 1;
constexpr int8_t kDefaultDb = 0;

// true, якщо значення balanceDb допустиме (у межах діапазону). Крок перевіряє лише UI / веб.
constexpr bool isValidDb(int v) {
    return v >= kMinDb && v <= kMaxDb;
}

// Значення з NVS / запиту -> допустиме: поза діапазоном -> kDefaultDb (0).
constexpr int8_t sanitizeDb(int v) {
    return isValidDb(v) ? static_cast<int8_t>(v) : kDefaultDb;
}

static_assert(kMinDb == -kMaxDb, "balance range must be symmetric");
static_assert(kStepDb >= 1, "balance step must be >= 1 dB");
static_assert(kDefaultDb >= kMinDb && kDefaultDb <= kMaxDb, "default balance must be in range");
static_assert(kDefaultDb == 0, "default balance is the neutral position (0 dB on both channels)");
static_assert(sanitizeDb(21) == kDefaultDb && sanitizeDb(-21) == kDefaultDb && sanitizeDb(-20) == -20 &&
                  sanitizeDb(20) == 20,
              "balance validation: out of range -> 0");

}  // namespace balance_cfg
