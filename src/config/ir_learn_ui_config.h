#pragma once

// Константи навчання IR-пульта на рівні AppController/UI (Prompt 15).
// Таймаути самого навчання (очікування натискання, конфлікт) живуть у
// config/ir_config.h і тут НЕ дублюються.

#include <stdint.h>

namespace ir_learn_ui_cfg {

// Скільки мілісекунд пристрій показує фінальний результат навчання
// (Success «Learned!» або Timeout), перш ніж AppController поверне попередній
// режим. Явне скасування (cancelIrLearn / кнопки) повертає одразу, без паузи.
constexpr uint32_t kResultHoldMs = 1500;

static_assert(kResultHoldMs >= 500 && kResultHoldMs <= 5000,
              "result hold must be long enough to read and short enough not to block the device");

}  // namespace ir_learn_ui_cfg
