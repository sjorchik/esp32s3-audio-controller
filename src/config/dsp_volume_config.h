#pragma once

// Константи модуля audio/dsp_volume (Prompt 45): цифрова гучність і мʼют з лінійним переходом.
// Межі kMinDb / kMaxDb — лише захист від некоректних значень. Діапазон і крива гучності проєкту
// (послаблення line_path −60…−6 дБ; відображення кроків на дБ у P47) задаються окремо.

#include <stdint.h>

namespace dsp_volume_cfg {

// Перехід гучності (зміна послаблення, розмʼют): як раніше kUnmuteRampMs у line_path (100 мс).
constexpr uint32_t kRampMs = 100;

// Перехід до мʼюту: як раніше kMuteRampMs у line_path (20 мс).
constexpr uint32_t kMuteRampMs = 20;

// Захист: значення поза межами обрізаються. −120 дБ ≈ 1e−6 (практично тиша).
constexpr float kMinDb = -120.0f;
constexpr float kMaxDb = 0.0f;

// Частота дискретизації до першого setSampleRate().
constexpr uint32_t kDefaultSampleRateHz = 48000;

// Межі частоти дискретизації, що приймаються.
constexpr uint32_t kMinSampleRateHz = 8000;
constexpr uint32_t kMaxSampleRateHz = 96000;

static_assert(kRampMs > 0 && kMuteRampMs > 0, "ramps must be positive");
static_assert(kMinDb < kMaxDb && kMaxDb == 0.0f, "range must end at 0 dB");
static_assert(kDefaultSampleRateHz >= kMinSampleRateHz && kDefaultSampleRateHz <= kMaxSampleRateHz,
              "default sample rate must be inside the range");

}  // namespace dsp_volume_cfg