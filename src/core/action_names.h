#pragma once

// Імена логічних дій (Action) для веб-API й екрана навчання IR (Prompt 15).
// Чисті дані без стану: безпечно викликати з будь-якої задачі.
// Імена збігаються з тими, що пише IrRc5::exportJson() ("VOL_UP", "BASS_UP", ...).

#include <stddef.h>

#include "core/events.h"

namespace action_names {

struct Info {
    Action action;
    const char* name;
    // true: дію має сенс навчати з пульта. ENC_* (суто енкодер) — false.
    // OK і BACK теж true (змінено за рішенням власника; раніше MASTER SPEC розд. 5 їх виключав).
    bool learnable;
};

// Повна таблиця (усі відомі Action) у стабільному порядку.
size_t count();
const Info& at(size_t index);  // index < count()

// "?" для дії, якої немає в таблиці.
const char* name(Action action);

// Точний, регістрозалежний збіг з імʼям. false — невідоме імʼя.
bool fromName(const char* s, Action& out);

// false для дії, якої немає в таблиці, або не придатної для навчання.
bool isLearnable(Action action);

}  // namespace action_names
