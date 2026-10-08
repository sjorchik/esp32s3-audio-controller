#pragma once

// Веб-API керування (Prompt 18): живлення, мʼют, гучність, gain, вибір входу, плеєр,
// вибір станції. Усі зміни йдуть ЛИШЕ через AppController::runWebCommand(); модуль не
// чіпає AudioPlayer/AudioProcessor напряму (читає лише capabilities() для валідації).
// [Prompt 30] ВИНЯТОК: /api/eq (еквалайзер радіо) — чисто програмний DSP без заліза й без
// стану контролера: обробник напряму викликає eq::set*() і SettingsStore::modify().
//
// Маршрути (докладно — docs/web_api.md):
//   POST /api/power            {"state":"on"|"off"|"toggle"}
//   POST /api/mute             {"mute":true|false|"toggle"}
//   POST /api/volume           {"value":N}  або  {"step":N}
//   POST /api/gain             {"value":N}
//   POST /api/input            {"index":N}
//   POST /api/player/{play|pause|stop|toggle|next|prev}
//   POST /api/player/station   {"index":N[,"switchInput":true]}
//   GET  /api/eq               (P30) пресет еквалайзера радіо
//   POST /api/eq               (P30) {"gainsDb":[a,b,c,d,e]} або {"band":N,"gainDb":X}

#include <ESPAsyncWebServer.h>

class AudioProcessor;

// Реєструє маршрути на сервері. Викликати з web_server.cpp (registerRoutes) один раз.
// proc — той самий вказівник, що й у WebServerManager::begin() (nullptr дозволений:
// команди, яким потрібен чип, тоді відповідають 503 audio_unavailable).
void registerPlayerRoutes(AsyncWebServer& server, AudioProcessor* proc);
