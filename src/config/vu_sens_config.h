#pragma once

// Константи чутливості VU-метра (індивідуальної для кожного входу).
// [V1] Створено: чутливість VU = зсув рівня в дБ, що ДОДАЄТЬСЯ до виміряного рівня пікового індикатора
// ПЕРЕД відображенням (audio/vu_math.h::peakToNorm). На звук, EQ, гучність і баланс не впливає.
// Діапазон ±20 дБ, крок 1 дБ, за замовчуванням 0 (нейтрально: VU поводиться бітово як до V1).
// Додатне значення = VU реагує сильніше (показ піднімається), від'ємне = слабше.
// Значення зберігається в профілі входу (Settings::profiles[i].vuSensDb, NVS v8) і застосовується
// для АКТИВНОГО входу; для неактивного — лише запис у профіль (як баланс).

#include <stdint.h>

namespace vu_sens_cfg {

constexpr int8_t kMinDb     = -20;
constexpr int8_t kMaxDb     = 20;
constexpr int8_t kStepDb    = 1;
constexpr int8_t kDefaultDb = 0;

// true, якщо значення допустиме (у межах діапазону). Крок перевіряє лише UI / веб.
constexpr bool isValidDb(int v) {
    return v >= kMinDb && v <= kMaxDb;
}

// Значення з NVS / запиту -> допустиме: поза діапазоном -> kDefaultDb (0). Те саме правило, що balance_cfg.
constexpr int8_t sanitizeDb(int v) {
    return isValidDb(v) ? static_cast<int8_t>(v) : kDefaultDb;
}

// Обрізання до меж (для кроків енкодера / внутрішніх викликів, де значення не з зовнішнього джерела).
constexpr int8_t clampDb(int v) {
    return static_cast<int8_t>(v < kMinDb ? kMinDb : (v > kMaxDb ? kMaxDb : v));
}

static_assert(kMinDb == -kMaxDb, "VU sensitivity range must be symmetric");
static_assert(kStepDb >= 1, "VU sensitivity step must be >= 1 dB");
static_assert(kDefaultDb == 0, "default VU sensitivity is neutral (0 dB = behaviour before V1)");
static_assert(kDefaultDb >= kMinDb && kDefaultDb <= kMaxDb, "default VU sensitivity must be in range");
static_assert(sanitizeDb(21) == kDefaultDb && sanitizeDb(-21) == kDefaultDb && sanitizeDb(-20) == -20 &&
                  sanitizeDb(20) == 20,
              "VU sensitivity validation: out of range -> 0");
static_assert(clampDb(99) == kMaxDb && clampDb(-99) == kMinDb && clampDb(3) == 3, "clampDb bounds");

}  // namespace vu_sens_cfg
