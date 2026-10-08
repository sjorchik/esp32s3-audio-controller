#pragma once

// Константи модуля ui/screens (розкладка 320×170, кольори, marquee, standby).
// Шрифти: Large 24 px, Small 17 px, Tiny 14 px; іконка 24×24 (display_config.h).

#include <stdint.h>

#include "config/display_config.h"
#include "config/vu_config.h"

namespace screens_cfg {

constexpr int16_t kW = display_cfg::kWidth;
constexpr int16_t kH = display_cfg::kHeight;

// --- Загальне ---
constexpr int16_t kMargin   = 8;                  // бічні поля
constexpr int16_t kContentW = kW - 2 * kMargin;   // ширина області marquee/VU

// --- Кольори ---
constexpr uint16_t kColorAccent      = display_cfg::kColorYellow;
constexpr uint16_t kColorDim         = display_cfg::kColorGray;
constexpr uint16_t kColorOk          = display_cfg::kColorGreen;
constexpr uint16_t kColorBad         = display_cfg::kColorRed;
constexpr uint16_t kColorSelectionBg = display_cfg::rgb565(0, 70, 150);

// --- Standby ---
constexpr uint8_t kStandbyBrightnessPercent = 5;   // див. обґрунтування
constexpr int16_t kStandbyIconX    = (kW - display_cfg::kIconSize) / 2;
constexpr int16_t kStandbyIconY    = 73;
constexpr int16_t kStandbyCaptionY = 105;          // Tiny, по центру

// --- Верхній рядок (Radio, ExternalInput): іконки Wi-Fi / мʼюту ---
// [Prompt 33] Назву входу з екрана Radio прибрано: kTopBarTextY і kTopBarNameGap видалено.
constexpr int16_t kTopBarY     = 4;
constexpr int16_t kWifiIconX   = kW - kMargin - display_cfg::kIconSize;
constexpr int16_t kMuteIconX   = kWifiIconX - display_cfg::kIconSize - 6;
constexpr int16_t kOfflineMuteGap = 6;              // [Prompt 28] проміжок між позначкою "offline" і іконкою мʼюту

// --- Radio ---
constexpr int16_t kStationY   = 34;                // Large, marquee
// [Prompt 33] Обʼєднаний рядок: метадані (dim) АБО статус потоку (свій колір); Small, marquee.
// kStateY (старий окремий рядок статусу, y=122) прибрано; це місце порожнє.
constexpr int16_t kTrackY     = 62;
constexpr int16_t kVuX        = kMargin;           // [Prompt 17] область VU-метра
constexpr int16_t kVuY        = 88;
constexpr int16_t kVuW        = kContentW;
constexpr int16_t kVuH        = 30;

// --- [Prompt 17] VU: дві горизонтальні сегментні смуги (верхня L, нижня R) в області
// kVuX/Y/W/H. Ширина сегмента виводиться з кількості й проміжку; смуги центруються.
// Пороги кольору — vu_cfg::kYellowFrom/kRedFrom (частка шкали).
constexpr uint8_t  kVuSegments  = 25;
constexpr int16_t  kVuSegGap    = 2;
constexpr int16_t  kVuBarH      = 13;
constexpr int16_t  kVuBarGap    = kVuH - 2 * kVuBarH;                       // між смугами
constexpr int16_t  kVuSegW      = (kVuW - kVuSegGap * (kVuSegments - 1)) / kVuSegments;
constexpr int16_t  kVuUsedW     = kVuSegW * kVuSegments + kVuSegGap * (kVuSegments - 1);
constexpr int16_t  kVuBarX      = kVuX + (kVuW - kVuUsedW) / 2;
constexpr uint8_t  kVuYellowSeg = static_cast<uint8_t>(vu_cfg::kYellowFrom * kVuSegments);
constexpr uint8_t  kVuRedSeg    = static_cast<uint8_t>(vu_cfg::kRedFrom * kVuSegments);
constexpr uint16_t kColorVuOff  = display_cfg::rgb565(35, 35, 35);          // непідсвічений сегмент

// --- ExternalInput ---
constexpr int16_t kExtCaptionY = 40;               // Tiny "INPUT"
constexpr int16_t kExtNameY    = 62;               // Large, по центру

// --- Статус-рядок (Radio і ExternalInput) ---
constexpr int16_t kStatusLineY = 143;
constexpr int16_t kStatusY     = 148;              // Small: ціль зліва, значення справа

// --- [Prompt 32] Спливне вікно параметра звуку (Vol/Bass/Treble/Bal/Gain) ---
// Малюється ОСТАННІМ шаром поверх Radio/ExternalInput; центр екрана. Рамка — прямокутна
// (DisplayManager не має заокруглених примітивів). Статус-рядок (kStatusLineY/kStatusY) більше не
// малюється; константи лишено, щоб не чіпати static_assert нижче й зберегти розкладку для наступних кроків.
constexpr uint32_t kPopupTimeoutMs = 5000;       // після ОСТАННЬОЇ події, що показує вікно
constexpr uint8_t  kPopupMaxChars  = 3;          // найширше значення: "100", "+20", "-20"
// Метрики шрифту FontSize::Digits (em=112, DejaVu Sans; вивід tools/gen_digits_font.py):
constexpr int16_t  kPopupDigitsCellW    = 71;    // xAdvance цифри (моноширинний)
constexpr int16_t  kPopupDigitsInkH     = 86;    // висота цифри (піксельних рядків)
// Виміряно на залізі (P32, фото): LovyanGFX ставить верх цифри на верх рядка шрифту, тож відступ 0.
constexpr int16_t  kPopupDigitsTopInset = 0;     // від верху рядка шрифту до верху цифри
constexpr int16_t  kPopupDigitsDy       = 0;     // ручна підгонка вертикалі цифр (див. «Як перевірити»)
constexpr int16_t  kPopupBorder    = 2;
constexpr int16_t  kPopupPadX      = 16;
constexpr int16_t  kPopupPadTop    = 4;
constexpr int16_t  kPopupLabelGap  = 2;          // між підписом (Large) і цифрами
constexpr int16_t  kPopupPadBottom = 6;
constexpr int16_t  kPopupW = kPopupMaxChars * kPopupDigitsCellW + 2 * kPopupPadX + 2 * kPopupBorder;
constexpr int16_t  kPopupH = kPopupBorder + kPopupPadTop + display_cfg::kFontLargePx +
                             kPopupLabelGap + kPopupDigitsInkH + kPopupPadBottom + kPopupBorder;
constexpr int16_t  kPopupX = (kW - kPopupW) / 2;
constexpr int16_t  kPopupY = (kH - kPopupH) / 2;
constexpr int16_t  kPopupLabelY  = kPopupY + kPopupBorder + kPopupPadTop;
constexpr int16_t  kPopupDigitsY = kPopupLabelY + display_cfg::kFontLargePx + kPopupLabelGap -
                                   kPopupDigitsTopInset + kPopupDigitsDy;   // верх рядка шрифту цифр
constexpr uint16_t kColorPopupBg     = display_cfg::rgb565(12, 12, 20);
constexpr uint16_t kColorPopupBorder = display_cfg::kColorYellow;
constexpr uint16_t kColorPopupLabel  = display_cfg::kColorWhite;
constexpr uint16_t kColorPopupDigits = display_cfg::kColorYellow;

// --- Список станцій ---
constexpr int16_t kListTitleY   = 4;               // Small
constexpr int16_t kListLineY    = 26;
constexpr int16_t kListFirstY   = 30;
constexpr int16_t kListRowH     = 27;
constexpr uint8_t kListRows     = 5;
constexpr int16_t kListTextX    = 14;
constexpr int16_t kListTextDy   = 5;               // відступ тексту від верху рядка

// --- [Prompt 15] IrLearn: статус (Small) над назвою дії (Large), підказка (Small) під нею ---
constexpr int16_t kIrStatusY = 24;
constexpr int16_t kIrActionY = 62;
constexpr int16_t kIrHintY   = 108;

// --- [Prompt 16] OtaUpdate: заголовок Large, відсоток Large, смуга, підказка Small ---
constexpr int16_t kOtaTitleY   = 28;
constexpr int16_t kOtaPercentY = 64;
constexpr int16_t kOtaBarX     = kMargin;
constexpr int16_t kOtaBarY     = 106;
constexpr int16_t kOtaBarW     = kContentW;
constexpr int16_t kOtaBarH     = 16;
constexpr int16_t kOtaHintY    = 136;

// --- Плейсхолдер (Menu без контексту) ---
constexpr int16_t kPlaceholderY = 73;              // Large, по центру

// --- [Prompt 12] WifiSetup: заголовок Large, під ним SSID і адреса Small ---
constexpr int16_t kWifiSetupTitleY = 48;
constexpr int16_t kWifiSetupSsidY  = 88;
constexpr int16_t kWifiSetupIpY    = 112;
constexpr int16_t kWifiSetupHintY  = 140;   // [Prompt 28] Small: підказка "OK: work offline"

// --- Marquee ---
constexpr uint32_t kMarqueeStepMs      = 40;       // крок зсуву
constexpr int16_t  kMarqueeStepPx      = 2;        // 50 px/с
constexpr uint32_t kMarqueeEdgePauseMs = 1500;     // пауза на початку й у кінці

static_assert(kStatusY + display_cfg::kFontSmallPx <= kH, "status row out of screen");
static_assert(kVuY + kVuH < kStatusLineY, "VU frame overlaps status line");
static_assert(kTrackY + display_cfg::kFontSmallPx < kVuY, "info line overlaps VU frame");
static_assert(kStationY + display_cfg::kFontLargePx < kTrackY, "station overlaps info line");
static_assert(kListFirstY + kListRows * kListRowH <= kH, "station list out of screen");
static_assert(kMuteIconX > kMargin, "icons overflow top bar");
static_assert(kWifiSetupTitleY + display_cfg::kFontLargePx < kWifiSetupSsidY &&
                  kWifiSetupSsidY + display_cfg::kFontSmallPx < kWifiSetupIpY &&
                  kWifiSetupIpY + display_cfg::kFontSmallPx <= kH,
              "WifiSetup layout overlaps or leaves the screen");
static_assert(kWifiSetupIpY + display_cfg::kFontSmallPx < kWifiSetupHintY &&
                  kWifiSetupHintY + display_cfg::kFontSmallPx <= kH,
              "WifiSetup offline hint overlaps the address or leaves the screen");
static_assert(kIrStatusY + display_cfg::kFontSmallPx < kIrActionY &&
                  kIrActionY + display_cfg::kFontLargePx < kIrHintY &&
                  kIrHintY + display_cfg::kFontSmallPx <= kH,
              "IrLearn layout overlaps or leaves the screen");
static_assert(kOtaTitleY + display_cfg::kFontLargePx < kOtaPercentY &&
                  kOtaPercentY + display_cfg::kFontLargePx < kOtaBarY &&
                  kOtaBarY + kOtaBarH < kOtaHintY &&
                  kOtaHintY + display_cfg::kFontSmallPx <= kH,
              "OtaUpdate layout overlaps or leaves the screen");
static_assert(kMarqueeStepMs > 0 && kMarqueeStepPx > 0, "marquee step must be positive");
static_assert(kPopupW <= kW - 2 * kMargin, "popup is wider than the content area");
static_assert(kPopupH * 4 <= kH * 3, "popup is taller than 3/4 of the screen");
static_assert(kPopupDigitsY + kPopupDigitsTopInset + kPopupDigitsInkH <= kPopupY + kPopupH - kPopupBorder,
              "popup digits do not fit inside the frame");
static_assert(kPopupTimeoutMs > 0, "kPopupTimeoutMs must be > 0");
static_assert(kVuBarGap >= 0 && kVuSegW >= 2 && kVuUsedW <= kVuW,
              "VU bars do not fit into the VU area");
static_assert(kVuYellowSeg < kVuRedSeg && kVuRedSeg < kVuSegments,
              "VU color segment thresholds out of order");

}  // namespace screens_cfg