#pragma once

// Веб-сервер налаштувань і керування.
// Сторінки зберігаються в LittleFS, стиснуті gzip.
// Реалізація буде додана пізніше.

class WebServerManager {
public:
    // Ініціалізація сервера.
    static bool begin();

    // Періодична обробка.
    static void poll();
};