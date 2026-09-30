#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/audio_i2c.h"
#include "audio/audio_processor.h"

// Драйвер TDA7318 (ST): 4 стерео входи з підсиленням, гучність, бас/дискант,
// 4 атенюатори гучномовців (баланс/фейдер) з незалежним мʼютом. Loudness НЕМАЄ.
// Регістри й формат байтів — у tda7318.cpp та в таблиці у відповіді.
//
// Плавна зміна гучності (ramp): ЗРОБЛЕНА ВИКЛИКАЧЕМ, у драйвері її немає.
// Причина: драйвер не має ні задачі, ні таймера, а setVolume() не має блокувати.
// AppController сам кроком (у UI-одиницях, наприклад 1 крок / 5..10 мс) викликає
// setVolume(); кожен виклик коштує одну коротку I2C-транзакцію (≈0.3 мс), а
// повторні значення, що дають той самий регістр, у шину не йдуть (кеш).
//
// Мʼют = мʼют усіх чотирьох атенюаторів гучномовців (апаратний «швидкий притиск»,
// регістр гучності не чіпається). Послідовність «мʼют -> зміна входу/станції ->
// розмʼют» координує AppController; драйвер лише виконує окремі примітиви.
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

    bool probe() override;
    bool applyAll() override;
    uint32_t i2cErrorCount() const override;
    AudioProcessorState cachedState() const override;

private:
    // Індекси кеш-регістрів (див. audio_i2c::RegShadow).
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

    // Надсилає одним пакетом лише ті байти, що відрізняються від кешу.
    bool commit(const Pending* items, size_t count);
    bool commitSwitch();
    bool commitSpeakers();
    void fillSpeakers(Pending* out, bool forceMute) const;

    AudioProcessorState m_state = {};
    audio_i2c::RegShadow m_shadow;
    uint32_t m_errors = 0;   // пишеться під мʼютексом, читається без нього (32 біти)
    bool m_begun = false;
};
