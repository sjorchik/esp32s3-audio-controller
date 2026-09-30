#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/audio_i2c.h"
#include "audio/audio_processor.h"

// Драйвер PT2313L (Princeton): 3 стерео входи з підсиленням, гучність, бас/дискант
// і loudness (лише 28-pin корпус, див. audio_cfg::kPt2313lHasToneLoudness),
// 4 атенюатори гучномовців (баланс/фейдер) з незалежним мʼютом.
// Формат байтів збігається з TDA7318/TDA7313, крім аудіо-перемикача:
// там біт 2 = loudness (0 = УВІМКНЕНО), а крок підсилення 3.75 дБ.
//
// ВХОДІВ ТРИ, а не чотири: setInput(3) повертає false (capabilities().inputCount == 3).
//
// Ramp гучності й послідовність мʼюту — так само, як у Tda7318: ramp робить
// викликач (AppController), драйвер лише виконує примітиви, мʼют = мʼют атенюаторів.
class Pt2313l : public AudioProcessor {
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

    bool probe() override;
    bool applyAll() override;
    uint32_t i2cErrorCount() const override;
    AudioProcessorState cachedState() const override;

private:
    enum Reg : uint8_t {
        kRegVol,
        kRegSpkLf,
        kRegSpkRf,
        kRegSpkLr,
        kRegSpkRr,
        kRegSwitch,
        kRegBass,
        kRegTreble,
        kRegCount,
    };

    struct Pending {
        Reg reg;
        uint8_t value;
    };

    static_assert(kRegCount <= audio_i2c::RegShadow::kMaxRegs, "shadow too small");

    bool commit(const Pending* items, size_t count);
    bool commitSwitch();
    bool commitSpeakers();
    void fillSpeakers(Pending* out, bool forceMute) const;
    uint8_t switchByte() const;

    AudioProcessorState m_state = {};
    audio_i2c::RegShadow m_shadow;
    uint32_t m_errors = 0;   // пишеться під мʼютексом, читається без нього (32 біти)
    bool m_begun = false;
};
