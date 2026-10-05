#pragma once

// Serial-тестовий режим Settings (лише SETTINGS_TEST, config/settings_config.h).
// Усі команди мають префікс `set.` — рядки без нього ігноруються. Повний список:
// надіслати `set.help`.
//
//   set.show                  — друк поточних Settings і профілів входів, dirty, лічильник записів
//   set.<поле> <значення>     — змінити поле (відкладений запис, requestSave)
//        proc 0|1, bright 0..255, flip 0|1, bass/treble/balance -128..127,
//        loud 0|1, input 0..3, station 0..65535, vol -128..127, gain -128..127, mute 0|1
//        [Prompt 21b] bass/treble/balance/loud/vol/gain міняють ПРОФІЛЬ входу lastInput
//        (set.show друкує профілі всіх входів; поточний позначено «*»)
//   set.name <idx> <текст>    — назва входу idx (до 31 байта)
//   set.save                  — негайний запис
//   set.ramp                  — 100 requestSave() за ~2 с; друкує, скільки було записів
//   set.reset                 — дефолти в кеш + запис (чіпає лише наш ключ NVS)
//   set.erase                 — видалити ключ з NVS, кеш не чіпати (далі — перезавантажити,
//                               щоб перевірити «перший запуск»)
//   set.help
//
// Зміни НЕ застосовуються до працюючого заліза (немає AppController): processorType
// діє після перезавантаження.

#include <stdbool.h>

class SettingsTest {
public:
    // Запускає задачу, що читає Serial. false — задачу не створено.
    static bool begin();

    // Обробка одного рядка (без перевода рядка). Публічна, щоб майбутній спільний
    // Serial-роутер міг передавати сюди рядки з префіксом `set.`.
    static void handleLine(const char* line);
};
