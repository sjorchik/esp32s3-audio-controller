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
// [Prompt 47d] processFloat(): те саме множення з тим самим ramp на float-буфері (стерео), БЕЗ насичення
// (його робить викликач один раз у кінці конвеєра). process(int32_t*) для радіо без змін.
// [Prompt 48b] ДОДАНО: цифровий баланс. setBalanceDb(int8_t) (будь-яка задача, атомарне слово): додатне = вправо
// (ПОСЛАБЛЮЄТЬСЯ ЛІВИЙ канал на balanceDb дБ), від'ємне = вліво (послаблюється ПРАВИЙ), 0 = обидва без змін.
// Лише послаблення. Множники каналів — з таблиці, заповненої в конструкторі (process() без powf/double).
// Перехід множників — лінійний, посемпловий, balance_cfg::kRampMs; ціль міняється й посеред переходу (старт
// від поточного значення). Баланс застосовується ПІСЛЯ гучності, окремим проходом, і ЛИШЕ коли він не нейтральний
// (обидва множники 1.0 і переходу немає): при balanceDb == 0 process()/processFloat() БІТОВО як до P48b.
// forceMute()/requestForceMute() баланс не скидають (це налаштування, а не стан мʼюту).

#include <stdint.h>

#include <atomic>

#include "config/balance_config.h"
#include "config/dsp_volume_config.h"

class DspVolume {
public:
    DspVolume();  // [Prompt 48b] заповнює таблицю множників балансу

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

    // [Prompt 48b] будь-яка задача: баланс, дБ (обрізається до balance_cfg::kMinDb..kMaxDb). Перехід — у
    // наступному process(). Діє незалежно від мʼюту й гучності.
    void setBalanceDb(int8_t db);
    int8_t balanceDb() const;

    // --- ЛИШЕ задача-власник ---
    void forceMute();          // миттєво: мʼют, підсилення 0, перехід скасовано
    bool isRamping() const;    // триває перехід

    // Множення стерео-блоку int32 (L,R,L,R...) на місці. frames — кількість стереокадрів.
    void process(int32_t* buf, int32_t frames);

    // [Prompt 47d] Те саме на float-буфері (L,R,L,R...), без насичення.
    void processFloat(float* buf, int32_t frames);

private:
    bool prepare();                      // власник: запит force, нова ціль; false = блок не чіпати (0 дБ)
    void finish(float g, uint32_t rem);  // власник: зберегти стан і опублікувати silent_
    void resetState();  // власник: підсилення 0, перехід скасовано (мʼют не чіпає)
    bool balancePrepare();                               // [P48b] власник: нова ціль; false = баланс нейтральний
    void balanceApply(int32_t* buf, int32_t frames);     // [P48b] власник: множення каналів int32 з насиченням
    void balanceApplyFloat(float* buf, int32_t frames);  // [P48b] власник: те саме на float, без насичення

    uint32_t hz_ = dsp_volume_cfg::kDefaultSampleRateHz;

    // Задані значення (атомарні; читаються process()).
    std::atomic<float> attDb_{0.0f};
    std::atomic<float> attGain_{1.0f};
    std::atomic<bool>  mute_{true};        // стартує замʼюченим
    std::atomic<bool>  forceReq_{false};   // [P47a] запит миттєвого скидання (з будь-якої задачі)
    std::atomic<bool>  silent_{true};      // [P47a] публікує власник: підсилення 0 і переходу немає

    // [Prompt 48b] Баланс: задане значення (атомарне) і таблиця множників 10^(-k/20), k = 0..kMaxDb (після
    // конструктора лише читається).
    std::atomic<int8_t> balDb_{0};
    float balTab_[balance_cfg::kMaxDb + 1];

    // Стан власника.
    float    gCur_ = 0.0f;   // поточне підсилення
    float    gTgt_ = 0.0f;   // ціль поточного переходу
    float    step_ = 0.0f;   // приріст на кадр
    uint32_t rem_ = 0;       // кадрів до кінця переходу (0 = переходу немає)

    // [Prompt 48b] Стан балансу (власник): поточні й цільові множники каналів, приріст на кадр, кадрів до кінця.
    float    blCur_ = 1.0f, brCur_ = 1.0f;
    float    blTgt_ = 1.0f, brTgt_ = 1.0f;
    float    blStep_ = 0.0f, brStep_ = 0.0f;
    uint32_t bRem_ = 0;
};
