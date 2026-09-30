#pragma once
#include <stdint.h>

// Абстрактний інтерфейс аудіопроцесора (TDA7318 / PT2313L).
// [Prompt 3] Розширено: AudioProcType, probe(), applyAll(), i2cErrorCount(),
// cachedState(), поля fader/inputGain/balanceMin/balanceMax у capabilities,
// фабрика createAudioProcessor(). Наявні методи не змінювалися.

// Тип чіпа. Автовизначення неможливе (обидва на 0x44, зчитування немає),
// тому тип задається ззовні (згодом — з NVS/Settings: 0 = Tda7318, 1 = Pt2313l).
enum class AudioProcType : uint8_t {
    Tda7318,
    Pt2313l,
};

struct AudioProcessorCapabilities {
    bool bass;
    bool treble;
    bool balance;
    bool loudness;
    uint8_t inputCount;
    // Діапазони нижче — це шкала UI, а не сирі значення регістрів:
    //   volume  — 0..100 (крива в audio_cfg::volumeAttSteps),
    //   tone    — кроки по 2 дБ (audio_cfg::kToneStepDb),
    //   balance — кроки по 1.25 дБ; знак «+» = правий канал гучніший.
    int8_t volumeMin;
    int8_t volumeMax;
    int8_t toneMin;
    int8_t toneMax;
    // [Prompt 3] ДОДАНО
    // Окремий перед/зад: true лише якщо його можна керувати через цей інтерфейс.
    // Обидва чіпи мають 4 незалежні атенюатори (апаратно фейдер можливий),
    // але setFader() в інтерфейсі немає, тому обидва драйвери повертають false.
    bool fader;
    // Підсилення конкретного входу (задається в audio_cfg::kInputGainSteps).
    bool inputGain;
    int8_t balanceMin;
    int8_t balanceMax;
};

// [Prompt 3] ДОДАНО: останній ЗАПИТАНИЙ стан (не те, що гарантовано лежить у чіпі
// після збою шини — для цього є applyAll()).
struct AudioProcessorState {
    uint8_t input;
    int8_t volume;
    int8_t bass;
    int8_t treble;
    int8_t balance;
    bool mute;
    bool loudness;
};

// Усі методи потокобезпечні (спільний мʼютекс I2C-шини).
// Методи setXxx() повертають true, якщо команда виконана або не потрібна
// (значення вже у чіпі); false — шина не відповіла, непідтримувана функція
// або некоректний індекс входу. Значення поза діапазоном обрізаються до меж.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual bool begin() = 0;
    virtual bool setInput(uint8_t index) = 0;
    virtual bool setVolume(int8_t value) = 0;
    virtual bool setBass(int8_t value) = 0;
    virtual bool setTreble(int8_t value) = 0;
    virtual bool setBalance(int8_t value) = 0;
    virtual bool setMute(bool mute) = 0;
    virtual bool setLoudness(bool on) = 0;
    virtual AudioProcessorCapabilities capabilities() const = 0;

    // [Prompt 3] ДОДАНО
    // Чи відповідає щось на I2C-адресу процесора (ACK). Стан не змінює.
    virtual bool probe() = 0;
    // Повторно надіслати весь закешований стан (після standby / збою шини).
    virtual bool applyAll() = 0;
    // Скільки I2C-операцій запису не вдалося навіть після повторів (діагностика).
    virtual uint32_t i2cErrorCount() const = 0;
    // Останній запитаний стан.
    virtual AudioProcessorState cachedState() const = 0;
};

// [Prompt 3] ДОДАНО: фабрика. Обʼєкт створюється через new (не PSRAM) і живе
// весь час роботи прошивки. nullptr — тип вимкнено в config/features.h
// (ENABLE_TDA7318 / ENABLE_PT2313L) або не вистачило памʼяті.
AudioProcessor* createAudioProcessor(AudioProcType type);
