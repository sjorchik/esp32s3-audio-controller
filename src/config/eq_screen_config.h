#pragma once

// Константи екрана «EQ + баланс» (режим редагування еквалайзера й балансу АКТИВНОГО входу).
// [Prompt 49a] Створено: таймаут, крок, початковий вибір + розкладка ТИМЧАСОВОГО текстового вигляду
// (screens.cpp, drawEqBalance()). Справжнє малювання (6 вертикальних стовпчиків) — P49b: тоді блок
// «Тимчасовий вигляд» нижче буде замінено власним набором координат.
// Логіка (AppController) читає лише kTimeoutMs / kStepDb / kDefaultSelected.
// Serial-лог [EQS] керується прапорцем APP_CONTROLLER_DEBUG (config/app_controller_config.h).

#include <stdint.h>

#include "config/eq_config.h"

namespace eq_screen_cfg {

// Вихід з екрана без дій: від ОСТАННЬОЇ дії в екрані (UP / DOWN / LEFT / RIGHT / обертання / клік / VOL_*), мс.
constexpr uint32_t kTimeoutMs = 10000;

// Крок зміни вибраного параметра (смуга EQ і баланс), дБ на подію. UP / DOWN / обертання енкодера.
constexpr int kStepDb = 1;

// Параметр, вибраний при відкритті екрана: 0..kBandCount - 1 = смуги EQ, kBandCount = баланс.
constexpr uint8_t kDefaultSelected = 0;

// ---------------------------------------------------------------------------
// ТИМЧАСОВИЙ вигляд (P49a, лише для перевірки логіки на залізі; переписує P49b)
// ---------------------------------------------------------------------------
constexpr int16_t kTmpTitleY      = 4;    // Small: заголовок (і назва входу праворуч)
constexpr int16_t kTmpLineY       = 26;   // розділова лінія під заголовком
constexpr int16_t kTmpFirstY      = 30;   // верх першого рядка
constexpr int16_t kTmpRowH        = 23;   // висота рядка (6 рядків: 30 + 6 * 23 = 168 <= 170)
constexpr int16_t kTmpMarkerX     = 8;    // маркер ">" біля вибраного рядка
constexpr int16_t kTmpLabelX      = 26;   // підпис смуги / "Bal"
constexpr int16_t kTmpNameGap     = 12;   // мінімальний проміжок між заголовком і назвою входу

static_assert(kStepDb >= 1, "step must be at least 1 dB");
static_assert(kTimeoutMs >= 1000, "timeout is too short to use the screen");
static_assert(kDefaultSelected <= eq_cfg::kBandCount, "default selection out of range (0..bands = balance)");

}  // namespace eq_screen_cfg
