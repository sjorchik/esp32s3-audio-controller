#pragma once

// Енкодер на апаратному PCNT.
// Реалізація буде додана пізніше.

class Encoder {
public:
    // Ініціалізація PCNT і кнопки енкодера.
    static bool begin();

    // Періодична обробка.
    // Реалізація обере підхід пізніше.
    static void poll();
};