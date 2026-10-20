#pragma once

// Константи екрана «EQ + Balance» (режим редагування еквалайзера й балансу АКТИВНОГО входу).
// [Prompt 49a] Створено: таймаут, крок, початковий вибір, тимчасова текстова розкладка.
// [Prompt 49b] Тимчасова розкладка прибрана; додано справжню розкладку шести вертикальних стовпчиків
// (5 смуг EQ + баланс). Логіка (AppController) читає лише kTimeoutMs / kStepDb / kDefaultSelected —
// їх значення НЕ змінено.
// Усі числа екрана тут; screens.cpp не містить «магічних» координат.
// Розкладка (320×170):
//   y 0..23   шапка: назва «EQ + Balance» (Small) зліва, назва входу справа; лінія при kHeaderLineY
//   y 28..150 область стовпчиків (kAreaTop..kAreaBottom), нульова лінія при kZeroY
//   y 154..167 підписи частот і «Bal» (Tiny, kLabelY)

#include <stdint.h>

#include "config/balance_config.h"
#include "config/display_config.h"
#include "config/eq_config.h"
#include "config/screens_config.h"

namespace eq_screen_cfg {

// Вихід з екрана без дій: від ОСТАННЬОЇ дії в екрані (UP / DOWN / LEFT / RIGHT / обертання / клік / VOL_*), мс.
constexpr uint32_t kTimeoutMs = 10000;

// Крок зміни вибраного параметра (смуга EQ і баланс), дБ на подію. UP / DOWN / обертання енкодера.
constexpr int kStepDb = 1;

// Параметр, вибраний при відкритті екрана: 0..kBandCount - 1 = смуги EQ, kBandCount = баланс.
constexpr uint8_t kDefaultSelected = 0;

// ---------------------------------------------------------------------------
// Шапка
// ---------------------------------------------------------------------------
constexpr int16_t kHeaderTitleY   = 4;     // Small: заголовок і назва входу (одна лінія)
constexpr int16_t kHeaderLineY    = 24;    // розділова лінія під шапкою
constexpr int16_t kHeaderNameGap  = 12;    // мінімальний проміжок між заголовком і назвою входу
constexpr uint16_t kColorHeaderName = display_cfg::kColorYellow;   // назва входу — акцент (що саме редагується)

// ---------------------------------------------------------------------------
// Колонки (6 = 5 смуг EQ + баланс)
// ---------------------------------------------------------------------------
constexpr uint8_t kColumnCount = eq_cfg::kBandCount + 1;
constexpr uint8_t kBalanceColumn = eq_cfg::kBandCount;

constexpr int16_t kColW     = 38;   // ширина комірки стовпчика (разом з полем навколо бару)
constexpr int16_t kBarW     = 22;   // ширина самого стовпчика
constexpr int16_t kGapEq    = 12;   // проміжок між смугами EQ
constexpr int16_t kGapBal   = 24;   // проміжок перед балансом (з розділювальною лінією посередині)
constexpr int16_t kTotalW   = kColumnCount * kColW + (eq_cfg::kBandCount - 1) * kGapEq + kGapBal;
constexpr int16_t kLeft     = (display_cfg::kWidth - kTotalW) / 2;   // центрування по горизонталі

// Лівий край i-ї колонки. Перед балансом — (kBandCount - 1) проміжків EQ + kGapBal.
constexpr int16_t colX(uint8_t i) {
    return static_cast<int16_t>(kLeft + i * kColW +
                                (i < eq_cfg::kBandCount ? i * kGapEq
                                                        : (eq_cfg::kBandCount - 1) * kGapEq + kGapBal));
}

// Вертикальна розділова лінія між смугами EQ і балансом: по центру проміжку kGapBal.
constexpr int16_t kDividerX = static_cast<int16_t>(colX(kBalanceColumn) - kGapBal / 2);

// ---------------------------------------------------------------------------
// Вертикальна область і шкала
// ---------------------------------------------------------------------------
constexpr int16_t kAreaTop    = 28;                              // верх області стовпчиків
constexpr int16_t kAreaBottom = 150;                             // низ області (вище за підписи)
constexpr int16_t kZeroY      = (kAreaTop + kAreaBottom) / 2;    // нульова лінія (y=89)
// Піввисота області: повна шкала (±kEqFullDb для EQ, ±kBalFullDb для балансу — однакова висота).
constexpr int16_t kHalfH      = 40;
constexpr int     kEqFullDb   = eq_cfg::kGainMaxDb;    // 12
constexpr int     kBalFullDb  = balance_cfg::kMaxDb;   // 20

// Значення над / під стовпчиком: відступ від кінця бару, px.
constexpr int16_t kValueGap   = 2;
constexpr uint8_t kValueSmallPx = display_cfg::kFontSmallPx;   // вибране значення (жирніше за Tiny)
constexpr uint8_t kValueTinyPx  = display_cfg::kFontTinyPx;    // решта значень

// Підписи частот / «Bal» під областю.
constexpr int16_t kLabelY     = 154;

// ---------------------------------------------------------------------------
// Кольори
// ---------------------------------------------------------------------------
constexpr uint16_t kColorEqBar     = display_cfg::rgb565(90, 200, 255);   // EQ, вибране — холодний
constexpr uint16_t kColorEqBarDim  = display_cfg::rgb565(40, 80, 105);    // EQ, решта — приглушено
constexpr uint16_t kColorEqSel     = screens_cfg::kColorSelectionBg;     // підсвітка вибраної EQ-колонки
constexpr uint16_t kColorBalBar    = display_cfg::rgb565(255, 170, 0);    // баланс, вибране — амбер
constexpr uint16_t kColorBalBarDim = display_cfg::rgb565(110, 72, 0);     // баланс, решта
constexpr uint16_t kColorBalSel    = display_cfg::rgb565(80, 50, 0);      // підсвітка вибраної колонки балансу
constexpr uint16_t kColorBalValue  = display_cfg::rgb565(200, 140, 0);    // значення балансу, не вибране
constexpr uint16_t kColorZero      = screens_cfg::kColorDim;             // нульова лінія
constexpr uint16_t kColorDivider   = display_cfg::rgb565(60, 60, 60);    // розділювач EQ / Bal
constexpr uint16_t kColorValueDim  = screens_cfg::kColorDim;             // значення EQ, не вибране
constexpr uint16_t kColorValueSel  = display_cfg::kColorFg;              // значення вибраного
constexpr uint16_t kColorLabelDim  = screens_cfg::kColorDim;             // підписи, не вибрані
constexpr uint16_t kColorLabelSel  = display_cfg::kColorFg;              // підпис вибраного

// ---------------------------------------------------------------------------
// Перевірки розкладки (збій компіляції, якщо щось виходить за межі)
// ---------------------------------------------------------------------------
static_assert(kStepDb >= 1, "step must be at least 1 dB");
static_assert(kTimeoutMs >= 1000, "timeout is too short to use the screen");
static_assert(kDefaultSelected <= eq_cfg::kBandCount, "default selection out of range (0..bands = balance)");
static_assert(kHeaderLineY > kHeaderTitleY + display_cfg::kFontSmallPx, "header line overlaps the title");
static_assert(kAreaTop > kHeaderLineY, "columns area starts above the header line");
static_assert(kLeft >= 4 && kLeft + kTotalW <= display_cfg::kWidth - 4, "columns do not fit the screen width");
static_assert(kBarW >= 2 && kBarW < kColW, "bar must be narrower than its column");
static_assert(kDividerX > colX(eq_cfg::kBandCount - 1) + kColW && kDividerX < colX(kBalanceColumn),
              "divider must sit inside the gap before the balance column");
static_assert(kHalfH > 0, "half height must be positive");
static_assert(kZeroY - kHalfH - kValueGap - kValueSmallPx >= kAreaTop,
              "top value label (selected, full scale) leaves the columns area");
static_assert(kZeroY + kHalfH + kValueGap + kValueSmallPx <= kAreaBottom,
              "bottom value label (selected, full scale) leaves the columns area");
static_assert(kAreaBottom + 2 <= kLabelY && kLabelY + display_cfg::kFontTinyPx <= display_cfg::kHeight,
              "frequency labels leave the screen");
static_assert(kEqFullDb > 0 && kBalFullDb > 0, "full scale must be positive");

}  // namespace eq_screen_cfg