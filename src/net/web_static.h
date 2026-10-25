#pragma once

// Віддача вбудованих веб-сторінок (Prompt 19): для кожного запису kWebAssets
// реєструється GET-маршрут. Дані - gzip-масиви з flash (без копіювання в RAM).
// Викликати ПІСЛЯ реєстрації всіх /api/* маршрутів і ДО onNotFound().
// Працює лише на сервері режиму STA (web_server.cpp); captive portal в режимі AP
// має власний AsyncWebServer у wifi_manager.cpp і з цим модулем не перетинається.

class AsyncWebServer;

void registerStaticRoutes(AsyncWebServer& server);
