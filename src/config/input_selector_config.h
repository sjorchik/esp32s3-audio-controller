#pragma once

// Константи модуля audio/input_selector (реле-селектор зовнішніх входів).
// [Prompt 48a] Створено. Пін реле — лише в config/pins.h (pins::kInputRelay).
//
// Залізо: ОДНЕ DPDT-реле на ОДНОМУ GPIO комутує два стерео-джерела на вхід PCM1808:
//   реле вимкнено = логічний вхід 1 (TV Box), реле увімкнено = логічний вхід 2 (Computer).
// Радіо (логічний 0) реле НЕ чіпає. Рекомендація до плати (не код): pulldown (напр. 10 кОм) на затворі /
// базі драйвера реле, щоб до begin() і під час скидання реле гарантовано було вимкнено (TV Box).

#include <stdint.h>

namespace input_selector_cfg {

// true: рівень HIGH на піні = реле УВІМКНЕНО (Computer); false — інвертована схема (LOW = Computer).
// ПРИПУЩЕННЯ планування P48a (власник виправить, якщо розпаяно інакше).
constexpr bool kRelayActiveHigh = true;

// Пауза після перемикання реле (контакти й аналоговий сигнал на PCM1808 заспокоюються), перед розмʼютом.
// Замінює колишні 30 мс «чип-селектора» (app_controller_cfg::kExtInputSettleMs). Значення НЕ виміряне.
// Саму паузу тримає ВИКЛИКАЧ (AppController): модуль неблокуючий.
constexpr uint32_t kRelaySettleMs = 30;

// Логічні входи, які комутує реле.
constexpr uint8_t kInputTvBox    = 1;  // реле вимкнено
constexpr uint8_t kInputComputer = 2;  // реле увімкнено

static_assert(kRelaySettleMs >= 1, "relay settle time must be >= 1 ms");
static_assert(kInputTvBox != kInputComputer && kInputTvBox >= 1 && kInputComputer >= 1,
              "relay inputs are external logical inputs (1..)");

}  // namespace input_selector_cfg
