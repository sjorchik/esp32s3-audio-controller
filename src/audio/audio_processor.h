#pragma once

// Спільний абстрактний інтерфейс аудіопроцесора.
// Реалізації: TDA7318, PT2313L.

#include <stdint.h>

// Можливості конкретного процесора.
// Ховає в інтерфейсі непідтримувані функції.
struct AudioProcessorCapabilities {
    bool bass;
    bool treble;
    bool balance;
    bool loudness;

    uint8_t inputCount;

    int8_t volumeMin;
    int8_t volumeMax;
    int8_t toneMin;
    int8_t toneMax;
};

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    // Ініціалізація.
    virtual bool begin() = 0;

    // Вхід процесора: 0..3.
    virtual bool setInput(uint8_t index) = 0;

    // Гучність.
    virtual bool setVolume(int8_t value) = 0;

    // Тембр.
    virtual bool setBass(int8_t value) = 0;
    virtual bool setTreble(int8_t value) = 0;

    // Баланс.
    virtual bool setBalance(int8_t value) = 0;

    // Аттенюатор процесора.
    // Плавний мʼют і ramp під час перемикань.
    virtual bool setMute(bool mute) = 0;

    // Loudness, якщо підтримується.
    virtual bool setLoudness(bool on) = 0;

    // Можливості процесора.
    virtual AudioProcessorCapabilities capabilities() const = 0;
};