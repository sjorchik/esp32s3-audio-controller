#pragma once

// Wi-Fi менеджер.
// Режим: станція, за потреби точка доступу для налаштування.
// Якщо немає збереженої мережі або 3 невдалі спроби —
// режим AP "AudioCtrl-Setup", captive portal.
// Реалізація буде додана пізніше.

class WifiManager {
public:
    // Ініціалізація.
    static bool begin();

    // Періодична обробка.
    static void poll();
};