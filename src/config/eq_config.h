#pragma once

// Константи 5-смугового еквалайзера радіо (Prompt 30).
// Еквалайзер — цифровий каскад peaking-biquad (RBJ) у хуку audio_process_i2s
// (audio/vu_pcm_hook.cpp), діє ЛИШЕ на декодований потік інтернет-радіо. Зовнішні входи
// (TV Box / Computer / Aux) йдуть аналоговим трактом через TDA7318 / PT2313L і хук не минають.
// Один глобальний пресет (не прив'язаний ні до входу, ні до станції); зберігається в
// Settings::eqGainsDb. Піни й I2S цей модуль не чіпає.

#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог еквалайзера ("[EQ] ..."): лише зміни смуг (веб-задача) і зміни частоти
// дискретизації (задача плеєра). У гарячому шляху хука (process) логів немає.
#define EQ_DEBUG 1

// 1 = задача плеєра після кожного Audio::loop() читає Audio::getSampleRate() і передає її в
// eq::setSampleRate() (частота потоку змінна: 44.1 / 48 / 32 кГц ...).
// 0 = частота лишається eq_cfg::kDefaultSampleRateHz (поставити 0, якщо у вашій версії
// ESP32-audioI2S немає публічного getSampleRate() — звірити з Audio.h!).
#define EQ_SAMPLE_RATE_FROM_LIB 1

namespace eq_cfg {

// ---------------------------------------------------------------------------
// Смуги
// ---------------------------------------------------------------------------
constexpr uint8_t kBandCount = 5;

// Центральні частоти, Гц (за зростанням; інтервал ≈ 2 октави).
constexpr uint16_t kBandFreqHz[kBandCount] = {60, 250, 1000, 4000, 12000};

// Підсилення смуги, дБ (цілі кроки). 0 дБ = смуга прозора (не обчислюється).
constexpr int kGainMinDb  = -12;
constexpr int kGainMaxDb  = 12;
constexpr int kGainStepDb = 1;

// Добротність peaking-фільтра. 1.0 при інтервалі смуг ≈ 2 октави дає ширину ≈ 1.4 октави:
// сусідні смуги плавно перекриваються, без «ям» між ними й без надмірного перекриття.
constexpr double kQ = 1.0;

// ---------------------------------------------------------------------------
// Частота дискретизації потоку
// ---------------------------------------------------------------------------
// Використовується, доки плеєр не повідомив справжню (і якщо EQ_SAMPLE_RATE_FROM_LIB == 0).
constexpr uint32_t kDefaultSampleRateHz = 44100;

// Значення поза цими межами ігноруються (сміття від getSampleRate() до розбору заголовка).
constexpr uint32_t kMinSampleRateHz = 8000;
constexpr uint32_t kMaxSampleRateHz = 96000;

// Смуга з центром вище kMaxFreqRatio * fs вимикається (біля Найквіста форма АЧХ біквада
// спотворюється): 12 кГц працює від fs >= 26.7 кГц, тобто при 22.05 кГц ця смуга мовчить.
constexpr double kMaxFreqRatio = 0.45;

static_assert(kBandCount >= 1, "need at least one band");
static_assert(kGainMinDb < 0 && kGainMaxDb > 0, "range must contain 0 dB");
static_assert(kGainMinDb >= -127 && kGainMaxDb <= 127, "gain must fit int8_t");
static_assert(kGainStepDb >= 1, "step must be positive");
static_assert((kGainMaxDb - kGainMinDb) % kGainStepDb == 0, "range must be a multiple of step");
static_assert(kBandFreqHz[0] > 0 && kBandFreqHz[0] < kBandFreqHz[1] &&
                  kBandFreqHz[1] < kBandFreqHz[2] && kBandFreqHz[2] < kBandFreqHz[3] &&
                  kBandFreqHz[3] < kBandFreqHz[4],
              "band frequencies must be strictly ascending");
static_assert(kQ > 0.1 && kQ < 10.0, "Q out of sane range");
static_assert(kMinSampleRateHz > 0 && kMinSampleRateHz < kMaxSampleRateHz, "sample rate range");
static_assert(kDefaultSampleRateHz >= kMinSampleRateHz && kDefaultSampleRateHz <= kMaxSampleRateHz,
              "default sample rate must be inside the range");
static_assert(kMaxFreqRatio > 0.0 && kMaxFreqRatio < 0.5, "ratio must be below Nyquist");

}  // namespace eq_cfg
