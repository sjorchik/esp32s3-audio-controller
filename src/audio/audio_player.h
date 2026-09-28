#pragma once

// Інтернет-радіо на базі ESP32-audioI2S.
// Реалізація буде додана пізніше.

#include <Arduino.h>

class AudioPlayer {
public:
    // Ініціалізація аудіопідсистеми.
    static bool begin();

    // Запуск потоку за URL.
    static bool playUrl(const char* url);

    // Зупинка потоку.
    static bool stop();

    // Статус відтворення.
    static bool isPlaying();

    // Періодична обробка.
    // Має викликатися в окремій аудіо-задачі.
    static void taskLoop();
};