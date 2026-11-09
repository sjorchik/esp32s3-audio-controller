#include "core/action_names.h"

#include <string.h>

#include "config/features.h"  // [USB11] FEATURE_USB_MODE

namespace action_names {

namespace {

// Порядок: як у IR_LEARNING.txt (список навчання), далі технічні дії.
// [Prompt 49e] learnable = false також для дій без ефекту: INPUT_AUX (вхід AUX видалено) та
// зарезервованих BASS_* / TREBLE_* / GAIN_* (тембру й gain більше немає; AppController їх ігнорує).
// Самі записи й порядок лишаються (імена потрібні для exportJson / importJson та старих мап у NVS).
// BALANCE_UP / BALANCE_DOWN працюють (±1 дБ балансу активного входу) і лишаються learnable = true.
const Info kTable[] = {
    {Action::POWER, "POWER", true},
    {Action::UP, "UP", true},
    {Action::DOWN, "DOWN", true},
    {Action::LEFT, "LEFT", true},
    {Action::RIGHT, "RIGHT", true},
    {Action::VOL_UP, "VOL_UP", true},
    {Action::VOL_DOWN, "VOL_DOWN", true},
    {Action::MUTE, "MUTE", true},
    {Action::MENU, "MENU", true},
    {Action::INPUT_RADIO, "INPUT_RADIO", true},
    {Action::INPUT_TV, "INPUT_TV", true},
    {Action::INPUT_PC, "INPUT_PC", true},
    {Action::INPUT_AUX, "INPUT_AUX", false},  // [P49e] вхід AUX видалено
#if FEATURE_USB_MODE
    // [INP4a-2] Було USB_MODE. Прямий вибір входу «USB» (підпис для UI / вебу: «Вхід USB»; сама таблиця підписів не має -
    // українські підписи живуть у webui, див. USB16a): входить у режим; у режимі «USB» нічого не робить.
    {Action::INPUT_USB, "INPUT_USB", true},
    {Action::USB_SHUFFLE, "USB_SHUFFLE", true},  // [USB14] перемішування (лише в режимі «USB»)
    {Action::USB_REPEAT, "USB_REPEAT", true},    // [USB14] повтор усього (лише в режимі «USB»)
#endif
    {Action::DIGIT_0, "DIGIT_0", true},
    {Action::DIGIT_1, "DIGIT_1", true},
    {Action::DIGIT_2, "DIGIT_2", true},
    {Action::DIGIT_3, "DIGIT_3", true},
    {Action::DIGIT_4, "DIGIT_4", true},
    {Action::DIGIT_5, "DIGIT_5", true},
    {Action::DIGIT_6, "DIGIT_6", true},
    {Action::DIGIT_7, "DIGIT_7", true},
    {Action::DIGIT_8, "DIGIT_8", true},
    {Action::DIGIT_9, "DIGIT_9", true},
    {Action::BASS_UP, "BASS_UP", false},
    {Action::BASS_DOWN, "BASS_DOWN", false},
    {Action::TREBLE_UP, "TREBLE_UP", false},
    {Action::TREBLE_DOWN, "TREBLE_DOWN", false},
    {Action::BALANCE_UP, "BALANCE_UP", true},
    {Action::BALANCE_DOWN, "BALANCE_DOWN", true},
    {Action::GAIN_UP, "GAIN_UP", false},
    {Action::GAIN_DOWN, "GAIN_DOWN", false},
    // OK і BACK навчаються з пульта (за рішенням власника; раніше були false).
    {Action::OK, "OK", true},
    {Action::BACK, "BACK", true},
    // Не для навчання з пульта (суто енкодер):
    {Action::ENC_CW, "ENC_CW", false},
    {Action::ENC_CCW, "ENC_CCW", false},
    {Action::ENC_PRESS, "ENC_PRESS", false},
};

constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

const Info* find(Action a) {
    for (size_t i = 0; i < kTableSize; ++i) {
        if (kTable[i].action == a) return &kTable[i];
    }
    return nullptr;
}

}  // namespace

size_t count() { return kTableSize; }

const Info& at(size_t index) { return kTable[index < kTableSize ? index : 0]; }

const char* name(Action action) {
    const Info* i = find(action);
    return i != nullptr ? i->name : "?";
}

bool fromName(const char* s, Action& out) {
    if (s == nullptr) return false;
    for (size_t i = 0; i < kTableSize; ++i) {
        if (strcmp(kTable[i].name, s) == 0) {
            out = kTable[i].action;
            return true;
        }
    }
    return false;
}

bool isLearnable(Action action) {
    const Info* i = find(action);
    return i != nullptr && i->learnable;
}

}  // namespace action_names
