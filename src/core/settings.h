#pragma once

// Налаштування в NVS, namespace "audioctl".
// Реалізація буде додана пізніше.

#include <Arduino.h>
#include <stdint.h>

// Налаштування, які зберігаються між запуском.
struct Settings {
    // Тип аудіопроцесора: 0 = TDA7318, 1 = PT2313L.
    uint8_t processorType;

    // Назви входів.
    char inputNames[4][32];

    // Яскравість підсвітки дисплея.
    uint8_t brightness;

    // Останній стан для відновлення.
    uint8_t lastInput;
    uint16_t lastStation;
    int8_t lastVolume;
    bool lastMute;
};

class SettingsStore {
public:
    // Ініціалізація NVS.
    static bool begin();

    // Завантажити збережені налаштування.
    static bool load();

    // Зберегти налаштування.
    static bool save();

    // Доступ до поточного екземпляра.
    static Settings& get();
};