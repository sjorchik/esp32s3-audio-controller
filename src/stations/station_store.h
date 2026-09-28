#pragma once

// Сховище станцій у LittleFS, формат JSON.
// Імпорт: M3U / PLS / JSON.
// Експорт: JSON.
// Реалізація буде додана пізніше.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Опис станції.
struct Station {
    uint16_t id;
    char name[64];
    char url[192];
};

class StationStore {
public:
    // Ініціалізація сховища.
    static bool begin();

    // Кількість станцій.
    static size_t count();

    // Отримати станцію за індексом.
    static bool get(size_t index, Station& out);

    // Імпорт списків відтворення.
    static bool importM3u(const char* path);
    static bool importPls(const char* path);
    static bool importJson(const char* path);

    // Експорт.
    static bool exportJson(const char* path);
};