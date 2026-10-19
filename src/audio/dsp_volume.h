#pragma once

// Цифрова гучність і мʼют (Prompt 45, доповнено Prompt 47a). Клас-екземпляр; process() — ЛИШЕ з задачі-власника.
//
// Послаблення і мʼют зберігаються в атомарних словах: їх можна змінювати з будь-якої задачі.
// Перехід (лінійний, посемпловий) починається в process() і має довжину:
//   - dsp_volume_cfg::kMuteRampMs, якщо ціль — тиша (мʼют);
//   - dsp_volume_cfg::kRampMs в інших випадках (зміна послаблення, розмʼют).
// Швидке зникнення звуку і повільне повернення — навмисно (як раніше в line_path).
//
// Для аварійного мʼюту (Stall) і старту є forceMute(): миттєво, без переходу. Виклик із задачі-власника.
// [Prompt 47a] requestForceMute() — те саме, але з БУДЬ-ЯКОЇ задачі: ставить мʼют і прапорець, який
// process() виконає першим ділом (стан власника скидається там, а не з чужої задачі).
// [Prompt 47a] isSilent() тепер читає атомарний прапорець silent_, який публікує власник (process() /
// forceMute()), тож безпечний з будь-якої задачі. isRamping() лишається ЛИШЕ для власника.
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

    // [Prompt 47a] будь-яка задача: миттєвий мʼют, виконується на початку наступного process().
    void requestForceMute();

    // [Prompt 47a] будь-яка задача: мʼют і підсилення вже 0 (перехід завершено).
    bool isSilent() const;

    // --- ЛИШЕ задача-власник ---
    void forceMute();          // миттєво: мʼют, підсилення 0, перехід скасовано
    bool isRamping() const;    // триває перехід

    // Множення стерео-блоку int32 (L,R,L,R...) на місці. frames — кількість стереокадрів.
    void process(int32_t* buf, int32_t frames);

private:
    void resetState();  // власник: підсилення 0, перехід скасовано (мʼют не чіпає)

    uint32_t hz_ = dsp_volume_cfg::kDefaultSampleRateHz;

    // Задані значення (атомарні; читаються process()).
    std::atomic<float> attDb_{0.0f};
    std::atomic<float> attGain_{1.0f};
    std::atomic<bool>  mute_{true};        // стартує замʼюченим
    std::atomic<bool>  forceReq_{false};   // [P47a] запит миттєвого скидання (з будь-якої задачі)
    std::atomic<bool>  silent_{true};      // [P47a] публікує власник: підсилення 0 і переходу немає

    // Стан власника.
    float    gCur_ = 0.0f;   // поточне підсилення
    float    gTgt_ = 0.0f;   // ціль поточного переходу
    float    step_ = 0.0f;   // приріст на кадр
    uint32_t rem_ = 0;       // кадрів до кінця переходу (0 = переходу немає)
};