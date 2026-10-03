#pragma once

// Константи модуля ui/screens (розкладка 320×170, кольори, marquee, standby).
// Шрифти: Large 24 px, Small 17 px, Tiny 14 px; іконка 24×24 (display_config.h).

#include <stdint.h>

#include "config/display_config.h"

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

// --- Верхній рядок (Radio, ExternalInput): назва входу + іконки ---
constexpr int16_t kTopBarY     = 4;
constexpr int16_t kTopBarTextY = 7;                // Small по центру 24-px смуги
constexpr int16_t kWifiIconX   = kW - kMargin - display_cfg::kIconSize;
constexpr int16_t kMuteIconX   = kWifiIconX - display_cfg::kIconSize - 6;

// --- Radio ---
constexpr int16_t kStationY   = 34;                // Large, marquee
constexpr int16_t kTrackY     = 62;                // Small, marquee
constexpr int16_t kVuX        = kMargin;           // TODO (Prompt 13): VU meter
constexpr int16_t kVuY        = 88;
constexpr int16_t kVuW        = kContentW;
constexpr int16_t kVuH        = 30;
constexpr int16_t kStateY     = 122;               // Small: Playing / Stopped / No Wi-Fi

// --- ExternalInput ---
constexpr int16_t kExtCaptionY = 40;               // Tiny "INPUT"
constexpr int16_t kExtNameY    = 62;               // Large, по центру

// --- Статус-рядок (Radio і ExternalInput) ---
constexpr int16_t kStatusLineY = 143;
constexpr int16_t kStatusY     = 148;              // Small: ціль зліва, значення справа

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

// --- Плейсхолдер (Menu без контексту) ---
constexpr int16_t kPlaceholderY = 73;              // Large, по центру

// --- [Prompt 12] WifiSetup: заголовок Large, під ним SSID і адреса Small ---
constexpr int16_t kWifiSetupTitleY = 48;
constexpr int16_t kWifiSetupSsidY  = 88;
constexpr int16_t kWifiSetupIpY    = 112;

// --- Marquee ---
constexpr uint32_t kMarqueeStepMs      = 40;       // крок зсуву
constexpr int16_t  kMarqueeStepPx      = 2;        // 50 px/с
constexpr uint32_t kMarqueeEdgePauseMs = 1500;     // пауза на початку й у кінці

static_assert(kStatusY + display_cfg::kFontSmallPx <= kH, "status row out of screen");
static_assert(kStateY + display_cfg::kFontSmallPx < kStatusLineY, "state text overlaps status line");
static_assert(kVuY + kVuH < kStateY, "VU frame overlaps state text");
static_assert(kTrackY + display_cfg::kFontSmallPx < kVuY, "track overlaps VU frame");
static_assert(kStationY + display_cfg::kFontLargePx < kTrackY, "station overlaps track");
static_assert(kListFirstY + kListRows * kListRowH <= kH, "station list out of screen");
static_assert(kMuteIconX > kMargin, "icons overflow top bar");
static_assert(kWifiSetupTitleY + display_cfg::kFontLargePx < kWifiSetupSsidY &&
                  kWifiSetupSsidY + display_cfg::kFontSmallPx < kWifiSetupIpY &&
                  kWifiSetupIpY + display_cfg::kFontSmallPx <= kH,
              "WifiSetup layout overlaps or leaves the screen");
static_assert(kIrStatusY + display_cfg::kFontSmallPx < kIrActionY &&
                  kIrActionY + display_cfg::kFontLargePx < kIrHintY &&
                  kIrHintY + display_cfg::kFontSmallPx <= kH,
              "IrLearn layout overlaps or leaves the screen");
static_assert(kMarqueeStepMs > 0 && kMarqueeStepPx > 0, "marquee step must be positive");

}  // namespace screens_cfg