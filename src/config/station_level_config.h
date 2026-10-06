#pragma once

// Константи рівня виходу декодера по станціях (Prompt 25).
// Рівень — ЦИФРОВЕ послаблення PCM у хуку audio/vu_pcm_hook.cpp ДО запису в I2S
// (audio/output_trim.*). Лише послаблення: 0 дБ = сигнал без змін.
// Піни, I2S і налаштування в NVS цей модуль не чіпає.

#include <stdint.h>

namespace station_level_cfg {

// Межі рівня станції, дБ (крок 1 дБ). Лише послаблення: максимум 0.
constexpr int kLevelMinDb = -24;
constexpr int kLevelMaxDb = 0;

// Рівень за замовчуванням: нова станція без поля, станції зі старого файлу, імпорт M3U / PLS.
constexpr int kDefaultStationLevelDb = -6;

// За скільки стереокадрів лінійний коефіцієнт проходить ВЕСЬ діапазон 0..1 (плавний
// перехід без клацань). 1536 кадрів ≈ 32-35 мс при 44.1-48 кГц; перехід на менший
// відрізок (наприклад -6 -> -10 дБ) відповідно коротший.
constexpr int32_t kTrimRampFrames = 1536;

static_assert(kLevelMinDb < kLevelMaxDb, "level range inverted");
static_assert(kLevelMaxDb == 0, "attenuation only: max must be 0 dB");
static_assert(kDefaultStationLevelDb >= kLevelMinDb && kDefaultStationLevelDb <= kLevelMaxDb,
              "default level must be inside the range");
static_assert(kLevelMinDb >= -127 && kLevelMaxDb <= 127, "level must fit int8_t");
static_assert(kTrimRampFrames > 0, "kTrimRampFrames must be positive");

}  // namespace station_level_cfg
