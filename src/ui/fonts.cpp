#include "ui/fonts.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config/display_config.h"
#include "ui/font_data.h"  // згенеровано tools/gen_gfxfont.py
#include "ui/font_digits_data.h"  // [Prompt 32] згенеровано tools/gen_digits_font.py

// ---------------------------------------------------------------------------
// Покриття (перевірено на залізі, фото тестового кадру):
//  - вбудований efontJA: НЕМАЄ Ґ ґ Є є І і Ї ї (показує «рамки»), решта
//    кирилиці є — тому efontJA замінено;
//  - DejaVu Sans (цей файл): є все, перевірено і за таблицею cmap, і на
//    прев'ю, зібраному з байтів згенерованих масивів (tools/font_preview.png).
//
// Регенерація (інший TTF, інший розмір, інші діапазони):
//   1. Покласти TTF у tools/fonts/ (з ліцензією, що дозволяє вбудовування;
//      DejaVu Sans — так).
//   2. Розміри (em, пікселів) — список SIZES у tools/gen_gfxfont.py, діапазони
//      кодів — RANGES там само. Потрібен Python 3 + Pillow.
//   3. Виконати з кореня проєкту:
//        python3 tools/gen_gfxfont.py --ttf tools/fonts/DejaVuSans.ttf
//            --out src/ui/font_data.h --preview tools/font_preview.png
//      (одним рядком)
//   4. Звірити надруковані скриптом yAdvance із display_cfg::kFont*Px
//      (UiFonts::begin() також друкує реальні висоти в Serial).
//
// [Prompt 32] Шрифт цифр спливного вікна — ОКРЕМИЙ файл ui/font_digits_data.h:
//   python3 tools/gen_digits_font.py --ttf tools/fonts/DejaVuSans.ttf --out src/ui/font_digits_data.h --em 112
// Конструктор lgfx::GFXfont звірено з lgfx_fonts.hpp вашої версії LovyanGFX
// (див. блок «Обʼєкти шрифтів»). Незвірено на залізі: чи знаходить бібліотека
// гліфи вище 0xFF (кирилиця) — це видно на тестовому кадрі DISPLAY_DEMO.
// ---------------------------------------------------------------------------

namespace {

bool s_ready = false;
SemaphoreHandle_t s_mutex = nullptr;

// Службовий спрайт без буфера: лише для textWidth()/fontHeight().
lgfx::LGFX_Sprite s_meter;

// --- Обʼєкти шрифтів ---
// Конструктор lgfx::GFXfont у LovyanGFX приймає НЕконстантні uint8_t* та
// GFXglyph* (перевірено збіркою), хоча шрифт лише читається. Масиви лежать у
// flash (const), тому const_cast безпечний: бібліотека їх не змінює.
// Решта параметрів конструктора (діапазони кодування) — за замовчуванням.
#define UI_MAKE_GFXFONT(prefix)                                                     \
    lgfx::GFXfont(const_cast<uint8_t*>(font_data::prefix##Bitmaps),                 \
                  const_cast<lgfx::GFXglyph*>(font_data::prefix##Glyphs),           \
                  font_data::prefix##First, font_data::prefix##Last,                \
                  font_data::prefix##YAdvance)

const lgfx::GFXfont s_fontLarge = UI_MAKE_GFXFONT(kLarge);
const lgfx::GFXfont s_fontSmall = UI_MAKE_GFXFONT(kSmall);
const lgfx::GFXfont s_fontTiny  = UI_MAKE_GFXFONT(kTiny);
const lgfx::GFXfont s_fontDigits = UI_MAKE_GFXFONT(kDigits);  // [Prompt 32]

#undef UI_MAKE_GFXFONT

static_assert(font_data::kLargeYAdvance == display_cfg::kFontLargePx,
              "display_cfg::kFontLargePx must equal Large yAdvance in font_data.h");
static_assert(font_data::kSmallYAdvance == display_cfg::kFontSmallPx,
              "display_cfg::kFontSmallPx must equal Small yAdvance in font_data.h");
static_assert(font_data::kTinyYAdvance == display_cfg::kFontTinyPx,
              "display_cfg::kFontTinyPx must equal Tiny yAdvance in font_data.h");
static_assert(font_data::kDigitsYAdvance == display_cfg::kFontDigitsPx,
              "display_cfg::kFontDigitsPx must equal Digits yAdvance in font_digits_data.h");

const lgfx::IFont* fontFor(FontSize size) {
    switch (size) {
        case FontSize::Large: return &s_fontLarge;
        case FontSize::Small: return &s_fontSmall;
        case FontSize::Tiny:  return &s_fontTiny;
        case FontSize::Digits: return &s_fontDigits;  // [Prompt 32]
    }
    return &s_fontSmall;
}

}  // namespace

bool UiFonts::begin() {
    if (s_ready) {
        return true;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        Serial.println("[FONT] mutex create failed");
        return false;
    }
    s_ready = true;

    const int32_t hLarge = lineHeight(FontSize::Large);
    const int32_t hSmall = lineHeight(FontSize::Small);
    const int32_t hTiny  = lineHeight(FontSize::Tiny);
    const int32_t hDigits = lineHeight(FontSize::Digits);  // [Prompt 32]
    Serial.printf("[FONT] DejaVu Sans GFXfont, line height: large=%d small=%d tiny=%d "
                  "(config %u/%u/%u)\n",
                  static_cast<int>(hLarge), static_cast<int>(hSmall), static_cast<int>(hTiny),
                  static_cast<unsigned>(display_cfg::kFontLargePx),
                  static_cast<unsigned>(display_cfg::kFontSmallPx),
                  static_cast<unsigned>(display_cfg::kFontTinyPx));

    Serial.printf("[FONT] digits: line height=%d (config %u), cell width=%d\n",
                  static_cast<int>(hDigits), static_cast<unsigned>(display_cfg::kFontDigitsPx),
                  static_cast<int>(textWidth("0", FontSize::Digits)));

    if (hLarge <= 0 || hSmall <= 0 || hTiny <= 0 || hDigits <= 0) {
        Serial.println("[FONT] font metrics invalid");
        s_ready = false;
        return false;
    }
    return true;
}

const lgfx::IFont* UiFonts::font(FontSize size) {
    return fontFor(size);
}

int32_t UiFonts::lineHeight(FontSize size) {
    if (!s_ready) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_meter.setFont(fontFor(size));
    const int32_t h = s_meter.fontHeight();
    xSemaphoreGive(s_mutex);
    return h;
}

int32_t UiFonts::textWidth(const char* utf8, FontSize size) {
    if (!s_ready || utf8 == nullptr) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_meter.setFont(fontFor(size));
    const int32_t w = s_meter.textWidth(utf8);
    xSemaphoreGive(s_mutex);
    return w;
}