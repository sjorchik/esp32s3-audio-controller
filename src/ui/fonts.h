#pragma once

// Українські шрифти для дисплея.
//
// Власні растрові шрифти GFXfont (1 біт/піксель) з DejaVu Sans, згенеровані
// tools/gen_gfxfont.py у ui/font_data.h. Покриття: ASCII, Latin-1, Latin
// Extended-A, уся кирилиця U+0400..U+045F і Ґ ґ (U+0490/0491) — тобто повний
// український алфавіт. Вбудовані шрифти LovyanGFX (efontJA) НЕ мають
// Ґ Є І Ї (підтверджено на залізі), тому не використовуються.
// Типографські символи (— – “ ” ’ …) у таблицю не входять: перед
// виведенням їх треба замінювати на ASCII-аналоги (ICY-нормалізація в
// майбутньому модулі).

#include <LovyanGFX.hpp>
#include <stdint.h>

// Розміри шрифта. Номінальна висота в пікселях — display_cfg::kFont*Px.
enum class FontSize : uint8_t {
    Large,  // назва станції, заголовок меню
    Small,  // статус-рядок, тембр
    Tiny,   // підписи, дрібні позначки
};

class UiFonts {
public:
    // Створює службовий обʼєкт виміру й перевіряє, що всі розміри доступні.
    // Викликається з DisplayManager::begin(); повторний виклик безпечний.
    static bool begin();

    // Шрифт для setFont() поверх спрайту. Ніколи не nullptr.
    static const lgfx::IFont* font(FontSize size);

    // Висота рядка шрифта, пікселів (0, якщо begin() не викликано).
    static int32_t lineHeight(FontSize size);

    // Ширина рядка UTF-8 у пікселях для обраного шрифта (для marquee в ui/screens).
    // Потокобезпечна (внутрішній мʼютекс); малювання не потребує.
    static int32_t textWidth(const char* utf8, FontSize size);
};
