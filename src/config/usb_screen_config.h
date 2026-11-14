#pragma once

// [USB17b] Розкладка екрана «зараз грає» режиму «USB» (ui/screens.cpp, drawUsb()). Дані — AppStateData::usbNow (USB17a).
// Шапка (іконки + «SSID  IP») — як на Radio / ExternalInput (screens_cfg::kTopBarY). Шрифти — наявні (Large / Small / Tiny).
// Позначки: ПРИПУЩЕННЯ — значення вибрано БЕЗ перевірки на дисплеї, підбирається за hwcheck_USB17b.md.

#include <stdint.h>

#include "config/display_config.h"
#include "config/screens_config.h"

namespace usb_screen_cfg {

namespace dc = display_cfg;
namespace sc = screens_cfg;

// Назва треку: Large, по центру, обрізається fitText() до kTextW (без marquee). Вертикаль (область під шапкою y = 28..170):
//   title 34..58 | artist 60..77 | час 90..114 | смуга 122..130 | нижній рядок 140..157.   ПРИПУЩЕННЯ.
constexpr int16_t kTextW   = sc::kContentW;                                         // 304
constexpr int16_t kTitleY  = sc::kExtAreaTop + 6;                                   // 34
constexpr int16_t kArtistY = kTitleY + dc::kFontLargePx + 2;                        // 60, Small, колір kColorTrack
constexpr int16_t kTimeY   = 90;                                                    // «mm:ss / mm:ss», Large, по центру
constexpr int16_t kBarY    = kTimeY + dc::kFontLargePx + 8;                         // 122
constexpr int16_t kBarH    = 8;
constexpr int16_t kBarX    = sc::kMargin;
constexpr int16_t kBarW    = sc::kContentW;
constexpr int16_t kInfoY   = kBarY + kBarH + 10;                                    // 140: нижній рядок («N / M», пауза, SHUF, REP)

// Індикатор паузи: дві вертикальні смужки (примітиви, нових іконок немає), по центру нижнього рядка.
constexpr int16_t kPauseBarW   = 4;
constexpr int16_t kPauseBarH   = 14;
constexpr int16_t kPauseBarGap = 4;

// Прапорці shuffle / repeat-all: Tiny, малюються ЛИШЕ коли ввімкнені; праве вирівнювання, REP крайній.
constexpr int16_t kFlagGap = 10;

// Не малювати назву / виконавця, доки не відомо хоч щось про трек (durSec > 0 або posSec > 0): на старті треку спершу
// приходить імʼя файла, потім теги (3 зміни тексту за ≈ 0,4 с, зі спостережень лога USB17a). Теги приходять до durSec,
// а posSec > 0 — запасний вихід, якщо тривалість не відома (VBR без заголовка): назва зʼявиться не пізніше за ≈ 1 с. ПРИПУЩЕННЯ.
constexpr bool kHoldTextUntilKnown = true;

// Макс. хвилин у «mm:ss» (довші треки показуються як 999:59, а не обрізаються).
constexpr uint32_t kMaxMinutes = 999;

// Кольори.
constexpr uint16_t kColorTitle  = dc::kColorFg;
constexpr uint16_t kColorArtist = sc::kColorTrack;
constexpr uint16_t kColorTime   = dc::kColorFg;
constexpr uint16_t kColorBarFrame = sc::kColorDim;
constexpr uint16_t kColorBarFill  = sc::kColorAccent;
constexpr uint16_t kColorInfo   = sc::kColorTrack;     // «N / M»
constexpr uint16_t kColorPause  = sc::kColorAccent;
constexpr uint16_t kColorFlagOn = sc::kColorAccent;

static_assert(kInfoY + dc::kFontSmallPx <= dc::kHeight, "usb screen: bottom row must fit the display");
static_assert(kTimeY > kArtistY + dc::kFontSmallPx, "usb screen: time row must not overlap the artist row");
static_assert(kBarY > kTimeY + dc::kFontLargePx, "usb screen: bar must not overlap the time row");
static_assert(kBarH >= 3, "usb screen: bar needs a frame and an inner fill");
static_assert(kPauseBarH <= dc::kFontSmallPx, "usb screen: pause bars must fit the bottom row");

}  // namespace usb_screen_cfg
