#pragma once

// Системні маршрути веб-API (Prompt 18):
//   GET  /api/system                 інформація про пристрій і прошивку
//   POST /api/system/reboot          відкладений перезапуск
//   POST /api/system/factory-reset   {"confirm":true}: налаштування -> заводські, перезапуск
//   POST /api/wifi/reset             {"confirm":true}: забути збережену мережу, перезапуск в AP
//
// OTA (POST /api/ota) лишається в web_server.cpp (Prompt 16) і тут не змінюється.
//
// Перезапуск виконує одноразова задача ПІСЛЯ відправки відповіді (config/web_api_config.h:
// kRestartDelayMs); з HTTP-обробника нічого не блокується. Під час OTA (Mode::OtaUpdate)
// усі три POST відповідають 409 ota_in_progress.

#include <ESPAsyncWebServer.h>

// Реєструє маршрути на сервері. Викликати з web_server.cpp (registerRoutes) один раз.
void registerSystemRoutes(AsyncWebServer& server);
