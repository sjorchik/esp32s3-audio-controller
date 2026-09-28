#pragma once

// Контролер логіки режимів і розкладки керування.
// Отримує події з EventBus і керує модулями через стан.
// Реалізація буде додана пізніше.

#include "core/events.h"

class AppController {
public:
    // Ініціалізація контролера.
    static bool begin();

    // Обробка однієї події.
    // Викликається задачею обробки шини подій.
    static void handleEvent(const Event& event);
};