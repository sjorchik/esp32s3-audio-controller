#pragma once

// [DEF5b] Запобіжна перевірка перед https-підключенням радіо після USB-сесії (WORKAROUND для DEF-5, НЕ виправлення:
// власник блока 448 Б, що розрізає вільну «діру», не встановлений — це DEF5c).
//
// Суть: після USB-сесії внутрішня купа може бути розрізана так, що два буфери TLS по 16 717 Б не вміщаються, хоча
// вільного місця ≈ 80 КБ; усі повтори підключення тоді падають ідентично до перезавантаження (`(-32512)`). Перевірка =
// ДВА пробні heap_caps_malloc(16 717, INTERNAL|8BIT), потім звільнення обох. Не вмістились навіть після очікування
// kMaxWaitMs — підключення НЕ починається, замовляється ОДИН тихий перезапуск (через AppController::prepareForRestart(),
// відновлюються той самий вхід і станція).
//
// Виклик: AudioPlayer::connectNow() (задача плеєра, ядро 1) — перед connecttohost() для https-станцій, якщо від
// завантаження була хоч одна USB-сесія (ssl_workaround::noteUsbExit() -> noteUsbSession()). http-станції пропускаються.
//
// Захист від петлі: (1) прапорець «була USB-сесія» — у RAM, скидається перезапуском, тож після перезапуску перевірка
// пропускається, доти поки знову не відбудеться USB-сесія; (2) один запит перезапуску за запуск; (3) обмежувач аптайму
// ssl_preflight_cfg::kMinUptimeMs. Прапорець RTC-блокування ssl_workaround сюди НЕ застосовується (рішення планування).
// При USB_HEAPMAP = 1 перевірка виконується й логується, але перезапуск НЕ робиться (діагностика DEF5c бачить збій).
//
// SSL_PREFLIGHT = 0 (config/features.h) або FEATURE_USB_HOST = 0: усе нижче — порожні inline-заглушки.

#include <stdint.h>

#include "config/features.h"

namespace ssl_preflight {

enum class Verdict : uint8_t {
    Proceed,           // підключатись як зазвичай
    RestartRequested,  // НЕ підключатись: тихий перезапуск замовлено (плеєр має перейти в Error із довгою паузою)
};

#if SSL_PREFLIGHT && FEATURE_USB_HOST

// Була USB-сесія в цьому запуску (викликає ssl_workaround::noteUsbExit() із задачі usb_mode).
void noteUsbSession();

// Рішення перед https-підключенням (url — поточна станція). Може БЛОКУВАТИ до kMaxWaitMs: лише з задачі плеєра.
Verdict beforeConnect(const char* url);

#if USB_HOST_DIAG
// Консоль: `usb.preflight test | force on | force off | stat` (див. usb_play_diag.cpp).
void handleConsole(const char* arg);
#endif

#else  // SSL_PREFLIGHT == 0 або немає USB-host

inline void noteUsbSession() {}
inline Verdict beforeConnect(const char*) { return Verdict::Proceed; }

#endif

}  // namespace ssl_preflight
