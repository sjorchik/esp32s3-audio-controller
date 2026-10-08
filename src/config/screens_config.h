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
// --- [Prompt 34] VU у стилі індикатора «Маяк-233» (ВЛЛ): дві сегментні смуги L (верх) / R (низ),
// між ними нерухома шкала в дБ. Мапінг рівня на сегменти лінійний за дБ (vu_cfg::kDbFloor..0).
// Смуги від лівого поля, підписи L/R праворуч від них; усе виводиться з констант нижче.
constexpr int16_t  kVuX          = kMargin;          // зона блоку (лише для ENABLE_VU == 0: рамка)
constexpr int16_t  kVuY          = 94;               // верх верхньої смуги (L)
constexpr int16_t  kVuW          = kContentW;
constexpr uint8_t  kVuSegments   = 41;               // сегментів на смугу
constexpr int16_t  kVuSegW       = 5;
constexpr int16_t  kVuSegGap     = 2;
constexpr int16_t  kVuSegPitch   = kVuSegW + kVuSegGap;
constexpr int16_t  kVuUsedW      = kVuSegments * kVuSegPitch - kVuSegGap;   // ширина смуги
constexpr int16_t  kVuBarX       = kMargin;          // лівий край смуг
constexpr int16_t  kVuLabelGap   = 6;                // між смугою і підписом L / R
constexpr int16_t  kVuLabelX     = kVuBarX + kVuUsedW + kVuLabelGap;   // підписи L / R (Tiny) праворуч
constexpr int16_t  kVuBarH       = 18;
constexpr int16_t  kVuRowGap     = 3;                // між смугою і рядком шкали
constexpr int16_t  kVuLY         = kVuY;
constexpr int16_t  kVuScaleY     = kVuLY + kVuBarH + kVuRowGap;                         // Tiny, підписи дБ
constexpr int16_t  kVuRY         = kVuScaleY + display_cfg::kFontTinyPx + kVuRowGap;
constexpr int16_t  kVuH          = kVuRY + kVuBarH - kVuY;                              // висота всього блоку
// Зона перевантаження: сегменти з номера kVuRedSeg (0-based) до кінця; поріг = vu_cfg::kRedFrom.
constexpr uint8_t  kVuRedSeg     = static_cast<uint8_t>(vu_cfg::kRedFrom * kVuSegments + 0.5f);
// Базовий рівень: у повній тиші (і без даних) на екрані Radio завжди світяться перші сегменти, як на ВЛЛ.
constexpr uint8_t  kVuMinSegments = 2;
// Утримання піку: один сегмент лишається яскравим kVuPeakHoldMs, далі спадає на сегмент за kVuPeakFallMs.
constexpr uint32_t kVuPeakHoldMs = 100;
constexpr uint32_t kVuPeakFallMs = 60;
// Мітки шкали, дБFS: мусять лежати в (vu_cfg::kDbFloor, 0] і зростати. Позиція = (дБ - kDbFloor) / (-kDbFloor).
constexpr int8_t   kVuScaleMarksDb[] = {-20, -14, -9, -5, -2, 0};
constexpr uint8_t  kVuScaleMarkCount = sizeof(kVuScaleMarksDb) / sizeof(kVuScaleMarksDb[0]);
constexpr bool vuMarksValid() {
    for (uint8_t i = 0; i < kVuScaleMarkCount; ++i) {
        if (kVuScaleMarksDb[i] <= vu_cfg::kDbFloor || kVuScaleMarksDb[i] > 0) return false;
        if (i > 0 && kVuScaleMarksDb[i] <= kVuScaleMarksDb[i - 1]) return false;
    }
    return true;
}
// Кольори: бірюзовий люмінофор ВЛЛ (за фото), перевантаження — червоний; непідсвічені — приглушені
// відтінки тих самих кольорів.
constexpr uint16_t kColorVuLit      = display_cfg::rgb565(0, 235, 215);
constexpr uint16_t kColorVuOver     = display_cfg::rgb565(255, 50, 40);
constexpr uint16_t kColorVuOff      = display_cfg::rgb565(10, 44, 46);
constexpr uint16_t kColorVuOverOff  = display_cfg::rgb565(52, 14, 12);
constexpr uint16_t kColorVuScale    = kColorVuLit;   // усі підписи VU того ж бірюзового, що й підсвічені сегменти

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
static_assert(kVuY + kVuH <= kH - 4, "VU block leaves the screen");
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
static_assert(kVuSegW >= 2 && kVuSegGap >= 1 && kVuLabelX + 12 <= kW - 4,
              "VU bars do not fit into the screen width");
static_assert(kVuMinSegments < kVuRedSeg, "VU base level must stay below the overload zone");
static_assert(kVuRedSeg > 0 && kVuRedSeg < kVuSegments, "VU overload segment out of range");
static_assert(kVuPeakHoldMs > 0 && kVuPeakFallMs > 0, "VU peak timings must be positive");
static_assert(kVuScaleMarkCount > 0 && vuMarksValid(), "VU scale marks must lie in (kDbFloor, 0] ascending");

}  // namespace screens_cfg