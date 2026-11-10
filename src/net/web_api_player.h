#pragma once

// Веб-API керування (Prompt 18): живлення, мʼют, гучність, вибір входу, плеєр,
// вибір станції. Усі зміни йдуть ЛИШЕ через AppController::runWebCommand(); модуль не
// чіпає AudioPlayer напряму. [P48a] AudioProcessor вирізано: registerPlayerRoutes() без proc.
// [Prompt 30] ВИНЯТОК: /api/eq (еквалайзер радіо) — чисто програмний DSP без заліза й без
// стану контролера: обробник напряму викликає eq::set*() і SettingsStore::modify().
//
// Маршрути (докладно — docs/web_api.md):
//   POST /api/power            {"state":"on"|"off"|"toggle"}
//   POST /api/mute             {"mute":true|false|"toggle"}
//   POST /api/volume           {"value":N}  або  {"step":N}
//   POST /api/input            {"index":N}  [USB15] у режимі «USB» -> 202 pending usb_exit (вихід із режиму й перехід на вхід);
//                              [INP4b] N = 0..3: 3 = вхід у режим «USB» -> 202 pending usb_enter (вимкнений / недоступний -> 409 input_disabled)
//   POST /api/player/{play|pause|stop|toggle|next|prev}
//   POST /api/player/station   {"index":N[,"switchInput":true]}
//   GET  /api/balance          (P50a) [?input=N] -> {"input":N,"balance":V}
//   POST /api/balance          (P48b) {"value":N[,"input":M]} баланс ±20 дБ активного (або вказаного) входу
//   GET  /api/vu               (V2) чутливість VU по входах: межі + значення всіх входів ([INP4b] 4)
//   POST /api/vu               (V2) {"value":N[,"input":M]} чутливість VU ±20 дБ активного (або вказаного) входу;
//                              лише відображення VU, на звук не впливає
//   GET  /api/eq               (P30) пресет еквалайзера радіо
//   POST /api/eq               (P30) {"gainsDb":[a,b,c,d,e]} або {"band":N,"gainDb":X}
//   GET  /api/inputs           (P39) входи: назва, апаратна доступність, дозвіл (маска); [INP4b] 4 входи (слот 3 = USB / «Player»)
//   POST /api/inputs           (P39) {"enabled":[b|null x4]} (x3 — ЗАСТАРІЛО, слот 3 не чіпається) або {"index":N,"enabled":bool};
//                              йде через AppController::runWebCommand(InputsEnabledSet)
// [INP4b] /api/eq, /api/balance, /api/vu приймають input 0..3 (слот 3 = профіль «Player»).

// [USB15] registerPlayerRoutes() заодно реєструє /api/usb/* (net/web_api_usb.*).

#include <ESPAsyncWebServer.h>
#include <stdint.h>

#include "config/settings_config.h"  // [INP4b] webUsbInputBuilt()
#include "core/app_state.h"          // [INP4b] AppStateData для webActiveInput()

// Реєструє маршрути на сервері. Викликати з web_server.cpp (registerRoutes) один раз.
void registerPlayerRoutes(AsyncWebServer& server);

// [INP4b] «Веб-активний вхід» — ЄДИНА функція для status.input, /api/inputs.input, відповідей /api/input і «поточного» входу в
// /api/eq, /api/balance, /api/vu: 3 (defaults::kUsbInputIndex), якщо опублікований стан режиму «USB» (AppStateData::usbUi) != Off
// (Starting / Ready / NoMedia / Fault / Stopping / Error), інакше AppStateData::inputIndex (0..2; поза межами -> 0).
// ГІПОТЕЗА / обмеження: usbUi похідний від usb_mode::state() і публікується контролером (publishState + періодично), тож
// може відставати від внутрішнього прапорця контролера на кілька десятків мс на межах входу / виходу.
uint8_t webActiveInput(const AppStateData& st);

// [INP4b] true, якщо USB-вхід зібрано (біт 3 маски допустимий): слот 3 тоді `available`. Compile-time.
inline constexpr bool webUsbInputBuilt() {
    return (settings_cfg::kInputMaskBuilt & settings_cfg::kInputMaskUsbBit) != 0;
}

