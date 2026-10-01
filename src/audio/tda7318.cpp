#include "audio/tda7318.h"

#include <Arduino.h>

#include "config/audio_config.h"
#include "config/defaults.h"

// Джерело: ST TDA7318, даташит листопад 1999 (cd00000150), розділ «Software specification».
// Адреса: байт 0x88 (1000100 + біт R/W = 0) = 7-бітна 0x44 (defaults::kAudioProcessorI2cAddr).
// Максимальна швидкість шини 100 кбіт/с. Зчитування немає.
//
// Кожен байт даних самоописний: старші біти = субадреса, тому кілька байтів
// можна слати в одній транзакції (адреса, байт, байт, ..., STOP).

namespace {

// --- Субадреси (старші біти байта) ---
constexpr uint8_t kSubVolume = 0x00;   // 00 B2 B1 B0 A2 A1 A0
constexpr uint8_t kSubSwitch = 0x40;   // 010 G1 G0 S2 S1 S0  (G = підсилення, S = вхід)
constexpr uint8_t kSubBass   = 0x60;   // 0110 C3 C2 C1 C0
constexpr uint8_t kSubTreble = 0x70;   // 0111 C3 C2 C1 C0
constexpr uint8_t kSubSpkLf  = 0x80;   // 100 B1 B0 A2 A1 A0  (лівий передній)
constexpr uint8_t kSubSpkRf  = 0xA0;   // 101 ...             (правий передній)
constexpr uint8_t kSubSpkLr  = 0xC0;   // 110 ...             (лівий задній)
constexpr uint8_t kSubSpkRr  = 0xE0;   // 111 ...             (правий задній)

// Атенюатор гучномовця: 5 біт B1 B0 A2 A1 A0 = кількість кроків по 1.25 дБ (0..30);
// 11111 = мʼют.
constexpr uint8_t kSpkMuteCode = 0x1F;

// [Prompt 7] Підсилення входу: поле G1 G0 — 2 біти, сирі кроки 0..3 (крок 6.25 дБ).
constexpr int8_t kGainMin = 0;
constexpr int8_t kGainMax = 3;

constexpr uint8_t kMaxBatch = 8;   // найбільший пакет: applyAll, крок 2

constexpr int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Гучність: сам байт = кількість кроків атенюації (0b00BBBAAA = 8*B + A, 1.25 дБ/крок).
constexpr uint8_t encodeVolume(uint8_t attSteps) {
    return static_cast<uint8_t>(kSubVolume | (attSteps & 0x3F));
}

constexpr uint8_t encodeSpeaker(uint8_t sub, uint8_t attSteps) {
    return static_cast<uint8_t>(sub | (attSteps & 0x1F));
}

// Аудіо-перемикач: G1 G0 — підсилення (11 = 0 дБ, 10 = +6.25, 01 = +12.5, 00 = +18.75),
// S2 = 0, S1 S0 — вхід 0..3 (Stereo 1..4).
constexpr uint8_t encodeSwitch(uint8_t input, uint8_t gainSteps) {
    return static_cast<uint8_t>(kSubSwitch | ((3 - (gainSteps & 0x03)) << 3) | (input & 0x03));
}

// Тембр: кроки по 2 дБ, -7..+7. C3 = знак.
//   C3..C0: 0000 = -14 дБ ... 0110 = -2 дБ, 0111 = 0, 1111 = 0, 1110 = +2 дБ ... 1000 = +14 дБ.
constexpr uint8_t toneCode(int8_t steps) {
    return static_cast<uint8_t>(steps <= 0 ? 7 + steps : 15 - steps);
}

constexpr uint8_t encodeTone(uint8_t sub, int8_t steps) {
    return static_cast<uint8_t>(sub | toneCode(steps));
}

// Приклади з самого даташиту (перевірка кодування на етапі компіляції):
static_assert(encodeVolume(36) == 0x24, "datasheet: -45 dB = 0 0 1 0 0 1 0 0");
static_assert(encodeSpeaker(kSubSpkRf, 20) == 0xB4, "datasheet: RF -25 dB = 1 0 1 1 0 1 0 0");
static_assert(encodeSwitch(1, 2) == 0x49, "datasheet: Stereo 2, +12.5 dB = 0 1 0 0 1 0 0 1");
static_assert(encodeTone(kSubBass, -5) == 0x62, "datasheet: bass -10 dB = 0 1 1 0 0 0 1 0");
static_assert(encodeSpeaker(kSubSpkLf, kSpkMuteCode) == 0x9F, "LF mute");
static_assert(encodeTone(kSubTreble, 7) == 0x78 && encodeTone(kSubTreble, 0) == 0x77,
              "treble +14 dB / 0 dB");
// [Prompt 7] Межі gain для входу 0: 0 дБ = 0x58, +18.75 дБ = 0x40.
static_assert(encodeSwitch(0, kGainMin) == 0x58 && encodeSwitch(0, kGainMax) == 0x40,
              "gain field G1 G0 in bits 4:3");

constexpr uint8_t balanceAttLeft(int8_t b)  { return b > 0 ? static_cast<uint8_t>(b) : 0; }
constexpr uint8_t balanceAttRight(int8_t b) { return b < 0 ? static_cast<uint8_t>(-b) : 0; }

}  // namespace

bool Tda7318::begin() {
    if (!audio_i2c::begin()) {
        Serial.println("[AUDIO] TDA7318 I2C init failed");
        return false;
    }

    // Одноразова пауза після подачі живлення (не «delay» у логіці роботи:
    // виконується лише тут, у setup(); зазвичай нульова, бо millis() уже давно > порогу).
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
        m_state.input = (defaults::kDefaultInput < audio_cfg::kTda7318InputCount)
                            ? defaults::kDefaultInput : 0;
        m_state.volume = static_cast<int8_t>(clampInt(
            defaults::kDefaultVolume, audio_cfg::kVolumeUiMin, audio_cfg::kVolumeUiMax));
        m_state.bass = static_cast<int8_t>(clampInt(
            defaults::kDefaultBass, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax));
        m_state.treble = static_cast<int8_t>(clampInt(
            defaults::kDefaultTreble, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax));
        m_state.balance = static_cast<int8_t>(clampInt(
            defaults::kDefaultBalance, audio_cfg::kBalanceUiMin, audio_cfg::kBalanceUiMax));
        m_state.mute = defaults::kDefaultMute;
        m_state.loudness = false;   // у TDA7318 loudness немає
        // [Prompt 7] kInputGainSteps — лише СТАРТОВІ значення; далі керує setGain().
        for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
            m_gain[i] = static_cast<int8_t>(
                clampInt(audio_cfg::kInputGainSteps[i], kGainMin, kGainMax));
        }
        m_state.gain = m_gain[m_state.input];
        m_shadow.invalidateAll();
        m_begun = true;
    }

    const bool ok = applyAll();
    Serial.printf("[AUDIO] TDA7318 begin %s\n", ok ? "ok" : "FAILED");
    return ok;
}

bool Tda7318::setInput(uint8_t index) {
    if (index >= audio_cfg::kTda7318InputCount) {
        return false;
    }
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.input = index;
    m_state.gain = m_gain[index];   // [Prompt 7] вхід повертає СВІЙ gain
    return commitSwitch();
}

bool Tda7318::setVolume(int8_t value) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.volume = static_cast<int8_t>(
        clampInt(value, audio_cfg::kVolumeUiMin, audio_cfg::kVolumeUiMax));
    const Pending p[] = {{kRegVol, encodeVolume(audio_cfg::volumeAttSteps(m_state.volume))}};
    return commit(p, 1);
}

bool Tda7318::setBass(int8_t value) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.bass = static_cast<int8_t>(
        clampInt(value, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax));
    const Pending p[] = {{kRegBass, encodeTone(kSubBass, m_state.bass)}};
    return commit(p, 1);
}

bool Tda7318::setTreble(int8_t value) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.treble = static_cast<int8_t>(
        clampInt(value, audio_cfg::kToneUiMin, audio_cfg::kToneUiMax));
    const Pending p[] = {{kRegTreble, encodeTone(kSubTreble, m_state.treble)}};
    return commit(p, 1);
}

bool Tda7318::setBalance(int8_t value) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.balance = static_cast<int8_t>(
        clampInt(value, audio_cfg::kBalanceUiMin, audio_cfg::kBalanceUiMax));
    return commitSpeakers();
}

bool Tda7318::setGain(int8_t value) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    const int8_t g = static_cast<int8_t>(clampInt(value, kGainMin, kGainMax));
    m_gain[m_state.input] = g;
    m_state.gain = g;
    return commitSwitch();   // той самий байт, що й вибір входу; кеш пропустить дублікат
}

bool Tda7318::setMute(bool mute) {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_state.mute = mute;
    return commitSpeakers();
}

bool Tda7318::setLoudness(bool on) {
    (void)on;
    // У TDA7318 loudness немає (біт S2 аудіо-перемикача має бути 0).
    return false;
}

AudioProcessorCapabilities Tda7318::capabilities() const {
    AudioProcessorCapabilities c = {};
    c.bass = true;
    c.treble = true;
    c.balance = true;
    c.loudness = false;
    c.inputCount = audio_cfg::kTda7318InputCount;
    c.volumeMin = audio_cfg::kVolumeUiMin;
    c.volumeMax = audio_cfg::kVolumeUiMax;
    c.toneMin = audio_cfg::kToneUiMin;
    c.toneMax = audio_cfg::kToneUiMax;
    c.fader = false;       // чип уміє, але в інтерфейсі setFader() немає
    c.inputGain = true;    // 0..+18.75 дБ, крок 6.25 дБ
    c.balanceMin = audio_cfg::kBalanceUiMin;
    c.balanceMax = audio_cfg::kBalanceUiMax;
    c.gainMin = kGainMin;  // сирі кроки 0..3
    c.gainMax = kGainMax;
    return c;
}

bool Tda7318::probe() {
    // Порожня транзакція на 0x44. PT2313L відповідає на ту саму адресу, тож це
    // підтверджує лише «на шині хтось є», а не що це саме TDA7318.
    return audio_i2c::probe();
}

bool Tda7318::applyAll() {
    audio_i2c::Lock lock;
    if (!lock.ok() || !m_begun) {
        return false;
    }
    m_shadow.invalidateAll();   // після збою/standby стан чипа невідомий

    // Крок 1 (окрема транзакція): заглушити всі гучномовці, поки виставляємо решту.
    // Окрема транзакція гарантує порядок незалежно від того, коли чип фіксує байти.
    Pending mute[4];
    fillSpeakers(mute, true);
    if (!commit(mute, 4)) {
        return false;
    }

    // Крок 2: гучність, тембр, вхід+gain, а тоді гучномовці зі справжнім значенням.
    // Якщо m_state.mute == true, їхні байти збігаються з уже надісланими й пропускаються.
    // Чип тримає gain лише активного входу; gain решти живе в m_gain[] і
    // підставляється при setInput().
    Pending spk[4];
    fillSpeakers(spk, false);
    const Pending all[kMaxBatch] = {
        {kRegVol, encodeVolume(audio_cfg::volumeAttSteps(m_state.volume))},
        {kRegBass, encodeTone(kSubBass, m_state.bass)},
        {kRegTreble, encodeTone(kSubTreble, m_state.treble)},
        {kRegSwitch, encodeSwitch(m_state.input, static_cast<uint8_t>(m_gain[m_state.input]))},
        spk[0], spk[1], spk[2], spk[3],
    };
    return commit(all, kMaxBatch);
}

uint32_t Tda7318::i2cErrorCount() const {
    return m_errors;
}

AudioProcessorState Tda7318::cachedState() const {
    audio_i2c::Lock lock;   // якщо не вдалося взяти — це лише діагностика, віддаємо як є
    return m_state;
}

bool Tda7318::commit(const Pending* items, size_t count) {
    uint8_t bytes[kMaxBatch];
    Reg regs[kMaxBatch];
    size_t n = 0;
    for (size_t i = 0; i < count && n < kMaxBatch; ++i) {
        if (m_shadow.isSame(items[i].reg, items[i].value)) {
            continue;   // у чипі вже це значення
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

bool Tda7318::commitSwitch() {
    const Pending p[] = {
        {kRegSwitch, encodeSwitch(m_state.input, static_cast<uint8_t>(m_gain[m_state.input]))}};
    return commit(p, 1);
}

bool Tda7318::commitSpeakers() {
    Pending spk[4];
    fillSpeakers(spk, false);
    return commit(spk, 4);   // зміни лівого/правого йдуть одним пакетом
}

void Tda7318::fillSpeakers(Pending* out, bool forceMute) const {
    const bool mute = forceMute || m_state.mute;
    const uint8_t l = mute ? kSpkMuteCode : balanceAttLeft(m_state.balance);
    const uint8_t r = mute ? kSpkMuteCode : balanceAttRight(m_state.balance);
    out[0] = {kRegSpkLf, encodeSpeaker(kSubSpkLf, l)};
    out[1] = {kRegSpkRf, encodeSpeaker(kSubSpkRf, r)};
    out[2] = {kRegSpkLr, encodeSpeaker(kSubSpkLr, l)};
    out[3] = {kRegSpkRr, encodeSpeaker(kSubSpkRr, r)};
}