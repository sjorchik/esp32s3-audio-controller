#pragma once
#include <stdint.h>

// Кнопки: POWER, UP, DOWN, LEFT, RIGHT, OK та кнопка енкодера.
// Опитування в окремій задачі FreeRTOS, події йдуть у EventBus.
class Buttons {
public:
    // Налаштовує піни й створює задачу опитування. Потребує готового EventBus.
    // Повторний виклик безпечний (повертає true, друга задача не створюється).
    static bool begin();

    // Скільки подій кнопок відкинуто через переповнену чергу EventBus.
    static uint32_t droppedEvents();

    // true, якщо пін безперервно тримається в активному стані (LOW) `ms` мс.
    // Працює ДО begin() і без EventBus (сам налаштовує пін як INPUT_PULLUP).
    // Блокує викликаючу задачу на час перевірки (до `ms` мс), тому
    // викликати лише зі setup().
    static bool isHeldAtBoot(uint8_t pin, uint32_t ms);
};
