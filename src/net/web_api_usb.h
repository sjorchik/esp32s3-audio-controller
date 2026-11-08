#pragma once

// [USB15] Веб-API плеєра USB (крок g2c-1; лише серверна частина: без webui/ і без екрана «зараз грає»).
// Докладний контракт — docs/web_api.md, §16. Модуль збирається лише при FEATURE_USB_MODE = 1 (config/features.h); при 0
// registerUsbRoutes() порожня, маршрутів /api/usb/* немає (404), поведінка пристрою як до кроку.
//
// Маршрути:
//   GET  /api/usb/status             легкий стан для опитування ≈ 2 Гц: режим, носій, cwd, список (версія), черга, «зараз
//                                    грає», lastReq (результат останньої команди)
//   GET  /api/usb/list?offset=&limit=[&version=]   сторінка записів {i, name, dir} поточної папки + version, nextOffset
//   GET  /api/usb/result?id=N        результат команди за reqId (кільце останніх 16)
//   POST /api/usb/mode               {"action":"enter"|"exit"|"toggle"}   (як RC5 USB_MODE; через AppController::runWebCommand)
//   POST /api/usb/cd                 {"index":N[,"version":V]} або {"up":true}
//   POST /api/usb/play               {"index":N[,"version":V]}
//   POST /api/usb/control            {"action":"pause"|"resume"|"toggle"|"next"|"prev"|"stop"}
//   POST /api/usb/seek               {"deltaSec":±N}
//   POST /api/usb/options            {"shuffle":bool,"repeat":bool} (одне чи обидва)
//
// Потоки. Обробники виконуються в задачі async_tcp і НЕ блокують: команди каталогу / черги лише ставляться в чергу usb_nav
// (результат — за reqId), гетери беруть мьютекс списку з таймаутом usb_browser_cfg::kApiLockWaitMs (20 мс; при таймауті —
// 503 busy). Читання флешки в обробниках не виконується.

#include <ESPAsyncWebServer.h>

// Реєструє маршрути /api/usb/*. Викликається з registerPlayerRoutes() (web_api_player.cpp) один раз.
void registerUsbRoutes(AsyncWebServer& server);
