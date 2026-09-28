#pragma once

// Іконки: Wi-Fi, мʼют, гучність, входи, standby тощо.
// Бітмапи в одному наборі: RGB565 або 1-біт.
// Реалізація буде додана пізніше.

#include <stdint.h>

enum class IconId : uint8_t {
    Wifi,
    Mute,
    Volume,
    Input,
    Standby,
};

class UiIcons {
public:
    // Ініціалізація набору іконок.
    static bool begin();
};