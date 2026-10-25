#pragma once

// Маршрути керування Wi-Fi у режимі STA без AP (Prompt 29):
//   GET    /api/wifi                    стан зʼєднання + збережені мережі (без паролів)
//   POST   /api/wifi/scan               запустити скан (асинхронно)
//   GET    /api/wifi/scan               стан і результати скану
//   POST   /api/wifi/networks           {ssid, password?, position?} додати / оновити
//   POST   /api/wifi/networks/move      {from, to} змінити пріоритет
//   DELETE /api/wifi/networks/{index}   видалити
//   POST   /api/wifi/connect            {index} негайно підключитись до збереженої мережі
// (POST /api/wifi/reset лишається в web_api_system.cpp.)
//
// Обробники не блокують AsyncTCP: список лише змінюється в RAM (запис у NVS — у задачі wifi),
// скан і підключення виконує задача WifiManager. Паролі ніколи не повертаються й не логуються.
// Мутуючі маршрути й скан відповідають 409 ota_in_progress / ir_learn_active під час OTA /
// навчання IR і 503 busy під час перезапуску.

#include <ESPAsyncWebServer.h>

// Реєструє маршрути; викликається з registerSystemRoutes() один раз.
void registerWifiRoutes(AsyncWebServer& server);
