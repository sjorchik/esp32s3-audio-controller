// Реле-селектор зовнішніх входів (TV Box / Computer). [Prompt 48a]
// Інтерфейс і правила використання — у audio/input_selector.h.

#include "audio/input_selector.h"

#include <Arduino.h>

#include "config/input_selector_config.h"
#include "config/pins.h"

namespace {

namespace cfg = input_selector_cfg;

bool s_ready = false;
uint8_t s_current = cfg::kInputTvBox;  // реле вимкнено після begin()

// Рівень піна для стану реле: relayOn = true -> реле увімкнено (Computer).
inline uint8_t levelFor(bool relayOn) {
    return (relayOn == cfg::kRelayActiveHigh) ? HIGH : LOW;
}

}  // namespace

namespace input_selector {

bool begin() {
    if (s_ready) {
        return true;
    }
    // Рівень «вимкнено» записуємо до і після переведення піна у вихід: реле одразу в TV Box, без
    // проміжного стану (зовнішній pulldown на драйвері реле тримає його вимкненим до цього моменту).
    digitalWrite(pins::kInputRelay, levelFor(false));
    pinMode(pins::kInputRelay, OUTPUT);
    digitalWrite(pins::kInputRelay, levelFor(false));
    s_current = cfg::kInputTvBox;
    s_ready = true;
    Serial.printf("[SEL] relay pin=%d activeHigh=%d -> TV Box\n", pins::kInputRelay,
                  cfg::kRelayActiveHigh ? 1 : 0);
    return true;
}

bool select(uint8_t logicalInput) {
    if (!s_ready) {
        return false;
    }
    if (logicalInput != cfg::kInputTvBox && logicalInput != cfg::kInputComputer) {
        return false;  // 0 (радіо) й будь-що інше: реле не чіпаємо
    }
    const bool relayOn = (logicalInput == cfg::kInputComputer);
    digitalWrite(pins::kInputRelay, levelFor(relayOn));
    s_current = logicalInput;
    Serial.printf("[SEL] input=%u relay=%s\n", static_cast<unsigned>(logicalInput), relayOn ? "on" : "off");
    return true;
}

uint8_t current() {
    return s_current;
}

}  // namespace input_selector
