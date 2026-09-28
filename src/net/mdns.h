#pragma once

// mDNS-реєстрація.
// Після підключення до мережі доступний за адресою audio.local.
// Реалізація буде додана пізніше.

class MdnsManager {
public:
    // Ініціалізація.
    static bool begin();
};