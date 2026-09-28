#pragma once

// Драйвер TDA7318.
// Реалізація буде додана пізніше.

#include "audio/audio_processor.h"

class Tda7318 : public AudioProcessor {
public:
    bool begin() override;
    bool setInput(uint8_t index) override;
    bool setVolume(int8_t value) override;
    bool setBass(int8_t value) override;
    bool setTreble(int8_t value) override;
    bool setBalance(int8_t value) override;
    bool setMute(bool mute) override;
    bool setLoudness(bool on) override;
    AudioProcessorCapabilities capabilities() const override;
};