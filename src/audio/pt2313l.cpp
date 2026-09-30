#include "audio/pt2313l.h"

#include <Arduino.h>

#include "config/audio_config.h"
#include "config/defaults.h"

// Джерело: Princeton PT2313L v1.6 (сент. 1999) + уривки таблиць регістрів PT2313L/PT2313.
// УВАГА: повних таблиць регістрів з оригінального PDF отримати не вдалося. Зі сторонніх
// витягів достовірно видно: субадреси (volume/speaker/switch/bass/treble), біти
// аудіо-перемикача та приклад «Stereo 1, +11.25 дБ, Loudness ON = 0100 0000».
// Кодування тембру (C3..C0), діапазони й код мʼюту атенюаторів взяті за аналогією з
// TDA7318/TDA7313 (чип pin-to-pin сумісний з TDA7313) — ЗВІРИТИ з даташитом.
//
// Адреса: 0x88 (запис) = 7-бітна 0x44. Швидкість шини — лише 100 кГц.

namespace {

// --- Субадреси (старші біти байта) — ті самі, що в TDA7318 ---
constexpr uint8_t kSubVolume = 0x00;   // 00 B2 B1 B0 A2 A1 A0
constexpr uint8_t kSubSwitch = 0x40;   // 010 G1 G0 L S1 S0   (L = loudness, 0 = УВІМК.)
constexpr uint8_t kSubBass   = 0x60;   // 0110 C3 C2 C1 C0    (лише 28-pin)
constexpr uint8_t kSubTreble = 0x70;   // 0111 C3 C2 C1 C0    (лише 28-pin)
constexpr uint8_t kSubSpkLf  = 0x80;   // 100 B1 B0 A2 A1 A0
constexpr uint8_t kSubSpkRf  = 0xA0;   // 101 ...
constexpr uint8_t kSubSpkLr  = 0xC0;   // 110 ...
constexpr uint8_t kSubSpkRr  = 0xE0;   // 111 ...

constexpr uint8_t kSpkMuteCode = 0x1F;   // B1 B0 A2 A1 A0 = 11111 (як у TDA7318) — ЗВІРИТИ
constexpr uint8_t kLoudnessOffBit = 0x04;

constexpr uint8_t kMaxBatch = 8;

constexpr int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr uint8_t encodeVolume(uint8_t attSteps) {
    return static_cast<uint8_t>(kSubVolume | (attSteps & 0x3F));
}

constexpr uint8_t encodeSpeaker(uint8_t sub, uint8_t attSteps) {
    return static_cast<uint8_t>(sub | (attSteps & 0x1F));
}

// G1 G0: 00 = +11.25 дБ, 01 = +7.5, 10 = +3.75, 11 = 0 дБ. Біт 2: 0 = loudness ON.
// S1 S0: 00..10 = Stereo 1..3 (11 = «Stereo 4», назовні не виведений — не використовується).
constexpr uint8_t encodeSwitch(uint8_t input, uint8_t gainSteps, bool loudnessOn) {
    return static_cast<uint8_t>(kSubSwitch | ((3 - gainSteps) << 3) |
                                (loudnessOn ? 0 : kLoudnessOffBit) | (input & 0x03));
}

// Тембр (за аналогією з TDA7318, кроки по 2 дБ, -7..+7) — ЗВІРИТИ.
constexpr uint8_t toneCode(int8_t steps) {
    return static_cast<uint8_t>(steps <= 0 ? 7 + steps : 15 - steps);
}

constexpr uint8_t encodeTone(uint8_t sub, int8_t steps) {
    return static_cast<uint8_t>(sub | toneCode(steps));
}

// Приклад з даташиту PT2313L: Stereo 1, +11.25 дБ, Loudness ON = 0 1 0 0 0 0 0 0.
static_assert(encodeSwitch(0, 3, true) == 0x40, "datasheet: Stereo 1, +11.25 dB, loudness ON");
static_assert(encodeSwitch(0, 0, false) == 0x5C, "Stereo 1, 0 dB, loudness OFF");
static_assert(encodeVolume(36) == 0x24, "-45 dB");
static_assert(encodeSpeaker(kSubSpkRf, 20) == 0xB4, "RF -25 dB");

constexpr uint8_t balanceAttLeft(int8_t b)  { return b > 0 ? static_cast<uint8_t>(b) : 0; }
constexpr uint8_t balanceAttRight(int8_t b) { return b < 0 ? static_cast<uint8_t>(-b) : 0; }

constexpr bool kHasTone = audio_cfg::kPt2313lHasToneLoudness;

}  // namespace

bool Pt2313l::begin() {
    if (!audio_i2c::begin()) {
        Serial.println("[AUDIO] PT2313L I2C init failed");
        return false;
    }

    // Даташит вимагає паузи після подачі живлення (для 20-pin версії з Cref = 10 мкФ — ≥300 мс).
    // Одноразово, лише тут; зазвичай нульова, бо millis() уже давно > порогу.
    const uint32_t now = millis();
    if (now < audio_cfg::kPowerOnSettleMs) {
        vTaskDelay(pdMS_TO_TICKS(audio_cfg::kPowerOnSettleMs - now));
    }

    {
        audio_i2c::Lock lock;
        if (!lock.ok()) {
            return false;
        }
        m_state = AudioProcessorState{};
        m_state.input = (defaults::kDefaultInput < audio_cfg::kPt2313lInputCount)
                            ? defaults::kDefaultInput : 0;
        m_state.volume = static_cast<int8_t>(clampInt(
            defaults::kDefaultVolume, audio_cfg::kVolumeUiMin, audio_cfg::kVolumeUiMax));
        m_state.bass = kHasTone ? static_cast<int8_t>(clampInt(
            defaults::kDefaultBass, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax)) : 0;
        m_state.treble = kHasTone ? static_cast<int8_t>(clampInt(
            defaults::kDefaultTreble, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax)) : 0;
        m_state.balance = static_cast<int8_t>(clampInt(
            defaults::kDefaultBalance, audio_cfg::kBalanceUiMin, audio_cfg::kBalanceUiMax));
        m_state.mute = defaults::kDefaultMute;
        m_state.loudness = kHasTone && defaults::kDefaultLoudness;
        m_shadow.invalidateAll();
        m_begun = true;
    }

    const bool ok = applyAll();
    Serial.printf("[AUDIO] PT2313L begin %s\n", ok ? "ok" : "FAILED");
    return ok;
}

bool Pt2313l::setInput(uint8_t index) {
    if (index >= audio_cfg::kPt2313lInputCount) {
        return false;   // у PT2313L лише 3 входи
    }
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.input = index;
    return commitSwitch();
}

bool Pt2313l::setVolume(int8_t value) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.volume = static_cast<int8_t>(
        clampInt(value, audio_cfg::kVolumeUiMin, audio_cfg::kVolumeUiMax));
    const Pending p[] = {{kRegVol, encodeVolume(audio_cfg::volumeAttSteps(m_state.volume))}};
    return commit(p, 1);
}

bool Pt2313l::setBass(int8_t value) {
    if (!kHasTone) {
        return false;
    }
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.bass = static_cast<int8_t>(
        clampInt(value, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax));
    const Pending p[] = {{kRegBass, encodeTone(kSubBass, m_state.bass)}};
    return commit(p, 1);
}

bool Pt2313l::setTreble(int8_t value) {
    if (!kHasTone) {
        return false;
    }
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.treble = static_cast<int8_t>(
        clampInt(value, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax));
    const Pending p[] = {{kRegTreble, encodeTone(kSubTreble, m_state.treble)}};
    return commit(p, 1);
}

bool Pt2313l::setBalance(int8_t value) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.balance = static_cast<int8_t>(
        clampInt(value, audio_cfg::kBalanceUiMin, audio_cfg::kBalanceUiMax));
    return commitSpeakers();
}

bool Pt2313l::setMute(bool mute) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.mute = mute;
    return commitSpeakers();
}

bool Pt2313l::setLoudness(bool on) {
    if (!kHasTone) {
        return false;   // 20-pin версія loudness не має
    }
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.loudness = on;
    return commitSwitch();   // біт loudness живе в тому самому байті, що й вибір входу
}

AudioProcessorCapabilities Pt2313l::capabilities() const {
    AudioProcessorCapabilities c = {};
    c.bass = kHasTone;
    c.treble = kHasTone;
    c.balance = true;
    c.loudness = kHasTone;
    c.inputCount = audio_cfg::kPt2313lInputCount;
    c.volumeMin = audio_cfg::kVolumeUiMin;
    c.volumeMax = audio_cfg::kVolumeUiMax;
    c.toneMin = kHasTone ? audio_cfg::kToneUiMin : 0;
    c.toneMax = kHasTone ? audio_cfg::kToneUiMax : 0;
    c.fader = false;       // чип уміє, але в інтерфейсі setFader() немає
    c.inputGain = true;    // 0..+11.25 дБ, крок 3.75 дБ
    c.balanceMin = audio_cfg::kBalanceUiMin;
    c.balanceMax = audio_cfg::kBalanceUiMax;
    return c;
}

bool Pt2313l::probe() {
    // Порожня транзакція на 0x44. TDA7318 відповідає на ту саму адресу, тож це
    // підтверджує лише «на шині хтось є», а не що це саме PT2313L.
    return audio_i2c::probe();
}

bool Pt2313l::applyAll() {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_shadow.invalidateAll();

    // Крок 1 (окрема транзакція): заглушити гучномовці на час виставлення решти.
    Pending mute[4];
    fillSpeakers(mute, true);
    if (!commit(mute, 4)) {
        return false;
    }

    // Крок 2: гучність, тембр (якщо є), вхід + loudness, гучномовці зі справжнім значенням.
    Pending spk[4];
    fillSpeakers(spk, false);
    Pending all[kMaxBatch];
    size_t n = 0;
    all[n++] = {kRegVol, encodeVolume(audio_cfg::volumeAttSteps(m_state.volume))};
    if (kHasTone) {
        all[n++] = {kRegBass, encodeTone(kSubBass, m_state.bass)};
        all[n++] = {kRegTreble, encodeTone(kSubTreble, m_state.treble)};
    }
    all[n++] = {kRegSwitch, switchByte()};
    for (uint8_t i = 0; i < 4; ++i) {
        all[n++] = spk[i];
    }
    return commit(all, n);
}

uint32_t Pt2313l::i2cErrorCount() const {
    return m_errors;
}

AudioProcessorState Pt2313l::cachedState() const {
    audio_i2c::Lock lock;   // діагностика: якщо мʼютекс не взято, віддаємо як є
    return m_state;
}

bool Pt2313l::commit(const Pending* items, size_t count) {
    uint8_t bytes[kMaxBatch];
    Reg regs[kMaxBatch];
    size_t n = 0;
    for (size_t i = 0; i < count && n < kMaxBatch; ++i) {
        if (m_shadow.isSame(items[i].reg, items[i].value)) {
            continue;
        }
        bytes[n] = items[i].value;
        regs[n] = items[i].reg;
        ++n;
    }
    if (n == 0) {
        return true;
    }
    if (!audio_i2c::write(bytes, n)) {
        for (size_t i = 0; i < n; ++i) {
            m_shadow.invalidate(regs[i]);
        }
        ++m_errors;
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        m_shadow.set(regs[i], bytes[i]);
    }
    return true;
}

uint8_t Pt2313l::switchByte() const {
    return encodeSwitch(m_state.input, audio_cfg::kInputGainSteps[m_state.input],
                        kHasTone && m_state.loudness);
}

bool Pt2313l::commitSwitch() {
    const Pending p[] = {{kRegSwitch, switchByte()}};
    return commit(p, 1);
}

bool Pt2313l::commitSpeakers() {
    Pending spk[4];
    fillSpeakers(spk, false);
    return commit(spk, 4);
}

void Pt2313l::fillSpeakers(Pending* out, bool forceMute) const {
    const bool mute = forceMute || m_state.mute;
    const uint8_t l = mute ? kSpkMuteCode : balanceAttLeft(m_state.balance);
    const uint8_t r = mute ? kSpkMuteCode : balanceAttRight(m_state.balance);
    out[0] = {kRegSpkLf, encodeSpeaker(kSubSpkLf, l)};
    out[1] = {kRegSpkRf, encodeSpeaker(kSubSpkRf, r)};
    out[2] = {kRegSpkLr, encodeSpeaker(kSubSpkLr, l)};
    out[3] = {kRegSpkRr, encodeSpeaker(kSubSpkRr, r)};
}
