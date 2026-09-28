#pragma once

// IR-приймач VS1838B, RMT RX.
// Протокол: RC5 / RC5X.
// У RC5X 7-й біт команди береться з інвертованого S2.
// Реалізація буде додана пізніше.

#include <stdint.h>

// Декодований код RC5/RC5X.
struct IrRc5Code {
    uint8_t address;
    uint8_t command;
    bool toggle;
};

class IrRc5 {
public:
    // Ініціалізація RMT і запуск прийому.
    static bool begin();

    // Спроба отримати декодований код.
    static bool read(IrRc5Code& code);

    // Вхід у режим навчання кнопок пульта.
    // Коди зберігаються в NVS.
    static void enterLearnMode();
};