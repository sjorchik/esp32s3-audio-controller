#pragma once

// mDNS-реєстрація.
// Після підключення до мережі пристрій доступний за адресою <defaults::kMdnsName>.local
// (audio.local) і анонсує сервіс _http._tcp на wifi_cfg::kMdnsHttpPort.
//
// [Prompt 12] Реалізовано. Сигнатура заглушки не змінена. Викликається ОДИН раз
// з WifiManager після успішного STA-підключення (не з main.cpp). Повторні виклики
// безпечні (повертають true, нічого не перезапускають).

class MdnsManager {
public:
    // Запускає MDNS.begin() і анонсує _http._tcp. Повертає false, якщо MDNS.begin() не вдався.
    static bool begin();
};
