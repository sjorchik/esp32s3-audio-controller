#pragma once

// Цифрова гучність і мʼют (Prompt 45). Клас-екземпляр; обробка ЛИШЕ з задачі-власника.
//
// Послаблення і мʼют зберігаються в атомарних словах: їх можна змінювати з будь-якої задачі.
// Перехід (лінійний, посемпловий) починається в process() і має довжину:
//   - dsp_volume_cfg::kMuteRampMs, якщо ціль — тиша (мʼют);
//   - dsp_volume_cfg::kRampMs в інших випадках (зміна послаблення, розмʼют).
// Швидке зникнення звуку і повільне повернення — навмисно (як раніше в line_path).
//
// Для аварійного мʼюту (Stall) і старту є forceMute(): миттєво, без переходу. Виклик із задачі-власника.
//
// Множення в float з насиченням на int32. При нульовому переході (0 дБ, без ramp) блок не чіпається.

#include <stdint.h>

#include <atomic>

#include "config/dsp_volume_config.h"

class DspVolume {
public:
    // --- ЛИШЕ задача-власник (або до її старту) ---
    void setSampleRate(uint32_t hz);

    // --- будь-яка задача: атомарні слова ---
    // db обрізається до dsp_volume_cfg::kMinDb..kMaxDb. Перехід починається в наступному process().
    void setAttenuationDb(float db);
    float attenuationDb() const;
    void setMute(bool mute);
    bool muted() const;

    // --- ЛИШЕ задача-власник ---
    void forceMute();          // миттєво: мʼют, підсилення 0, перехід скасовано
    bool isRamping() const;    // триває перехід
    bool isSilent() const;     // мʼют і підсилення вже 0 (перехід завершено)

    // Множення стерео-блоку int32 (L,R,L,R...) на місці. frames — кількість стереокадрів.
    void process(int32_t* buf, int32_t frames);

private:
    uint32_t hz_ = dsp_volume_cfg::kDefaultSampleRateHz;

    // Задані значення (атомарні; читаються process()).
    std::atomic<float> attDb_{0.0f};
    std::atomic<float> attGain_{1.0f};
    std::atomic<bool>  mute_{true};   // стартує замʼюченим

    // Стан власника.
    float    gCur_ = 0.0f;   // поточне підсилення
    float    gTgt_ = 0.0f;   // ціль поточного переходу
    float    step_ = 0.0f;   // приріст на кадр
    uint32_t rem_ = 0;       // кадрів до кінця переходу (0 = переходу немає)
};