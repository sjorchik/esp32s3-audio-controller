#pragma once

// [USB13b] WORKAROUND (НЕ виправлення) для відкритого DEF-5: після виходу з режиму «USB», у якому відтворювався
// файл, радіо (https) не підключається ((-32512) SSL - Memory allocation failed) і САМЕ не одужує до перезавантаження.
// Модуль робить ОДИН тихий перезапуск за запуск, якщо після такого виходу https-підключення радіо
// вдруге-втретє поспіль невдале (див. usb_host_cfg::kRestartWindowMs / kRestartFailCount).
//
// ВАЖЛИВО (ФАКТ із audio_player.cpp): текст «-32512» друкує ssl_client (лог Arduino-ESP32), а НЕ бібліотека аудіо через
// audio_info. AudioPlayer бачить лише connecttohost() == false (-> failStream("connecttohost failed")). Тому ознака
// події тут — «https-підключення радіо невдале N разів поспіль у вікні після виходу з файлом при живому Wi-Fi»;
// текст «32512» / «Memory allocation failed» у повідомленнях бібліотеки, якщо вона його колись перешле, лише ЛІЧИТЬСЯ й
// друкується в лозі (на рішення не впливає: поведінка однакова в усіх збірках).
//
// Захист від петлі: (1) не частіше одного разу за запуск (s_triggered); (2) прапорець у RTC_NOINIT-пам'яті (magic +
// інверсія), який ставиться ПЕРЕД ESP.restart() і зчитується / стирається на початку наступного запуску: якщо reset
// спричинив саме цей модуль (прапорець чинний І esp_reset_reason() == ESP_RST_SW), обхід у цьому запуску ВИМКНЕНО.
//
// Потоки: noteFilePlayed() / noteLibSslAllocText() / onRadioConnect() — з задачі плеєра; noteUsbExit() — із задачі
// usb_mode; onBoot() — з setup(). Перезапуск виконує окрема одноразова задача (prepareForRestart() блокує викликача).
// Модуль нічого не пише на носій і не чіпає NVS (це робить сам prepareForRestart()).

#include <stdint.h>

#include "config/features.h"

namespace ssl_workaround {

// Викликати ПЕРШИМ у setup() після Serial.begin(): читає й стирає RTC-прапорець попереднього запуску.
void onBoot();

// Файл успішно відкрито (AudioPlayer::doPlayFile після connecttoFS) — «файл грав у сеансі».
void noteFilePlayed();

// Режим «USB» завершує вихід: USB-host знято, зараз буде відновлено вхід (радіо стартує). Якщо в сеансі грав файл —
// озброює вікно kRestartWindowMs; прапорець «файл грав» споживається в будь-якому разі.
void noteUsbExit();

// Результат connecttohost() радіо (ok = повернене значення, url — поточна станція). Рішення про перезапуск — тут.
void onRadioConnect(bool ok, const char* url);

// Бібліотека аудіо надіслала рядок із «32512» / «Memory allocation failed» (лише лічильник і лог).
void noteLibSslAllocText();

#if FEATURE_USB_HOST && USB_HOST_DIAG
// Консоль: `usb.sslfail [dry | expired | stat]` (див. usb_play_diag.cpp). Без аргументу — ПОВНА імітація події з
// реальним перезапуском; dry — усе, крім перезапуску й запису RTC-прапорця; expired — вікно вже минуло (очікується
// відмова); stat — стан модуля.
void handleConsole(const char* arg);
#endif

}  // namespace ssl_workaround
