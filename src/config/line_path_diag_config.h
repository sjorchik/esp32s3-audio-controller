#pragma once
// Конфігурація діагностики line_path (Prompt PD1).
// Тимчасово увімкнено за замовчуванням для локалізації проблеми з ВЧ на зовнішніх входах.
// При 0 увесь діагностичний код (Goertzel, генератор тону, lp.raw) вимикається компілятором.

#include <stddef.h>  // Для size_t

#define LINE_PATH_DIAG 1

namespace line_path_diag_cfg {
// Частоти для аналізу спектра (алгоритм Герцеля), Гц.
constexpr float kGoertzelFreqs[] = {100.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 
                                    8000.0f, 12000.0f, 16000.0f, 20000.0f};
constexpr size_t kGoertzelCount = sizeof(kGoertzelFreqs) / sizeof(kGoertzelFreqs[0]);

// Параметри генератора тестового тону.
constexpr float kToneDefaultDbfs = -12.0f;
}  // namespace line_path_diag_cfg