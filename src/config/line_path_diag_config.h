#pragma once
// Конфігурація діагностики line_path (Prompt PD1; P48e).
// РЕЛІЗ = 0: увесь діагностичний код (Герцель, генератор тону, знімок lp.raw, lp.cfg / spec / bypass / tone,
// LinePath::diag*) компілюється геть. 1 — лише тимчасово для налагодження; консольні команди lp.* при цьому
// потребують ще й LINE_PATH_TEST == 1 (config/line_path_config.h).

#include <stddef.h>  // Для size_t
#include <stdint.h>

#define LINE_PATH_DIAG 0

namespace line_path_diag_cfg {
// Частоти для аналізу спектра (алгоритм Герцеля), Гц.
constexpr float kGoertzelFreqs[] = {100.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f,
                                    8000.0f, 12000.0f, 16000.0f, 20000.0f};
constexpr size_t kGoertzelCount = sizeof(kGoertzelFreqs) / sizeof(kGoertzelFreqs[0]);

// Параметри генератора тестового тону (lp.tone).
constexpr float kToneDefaultDbfs = -12.0f;
constexpr float kToneMinHz = 20.0f;
constexpr float kToneMaxHz = 20000.0f;

// lp.spec [сек]: тривалість вікна виміру.
constexpr uint32_t kSpecDefaultSec = 2;
constexpr uint32_t kSpecMinSec = 1;
constexpr uint32_t kSpecMaxSec = 10;
constexpr uint32_t kSpecPollMs = 50;         // період очікування в консольній задачі
constexpr uint32_t kSpecStopSettleMs = 20;   // пауза після зупинки вікна: задача line_path дообробляє поточний блок

// lp.raw [n]: скільки кадрів сирого RX друкувати (знімок робить задача line_path, статичний буфер 8 * n байт).
constexpr uint32_t kRawDefaultFrames = 16;
constexpr uint32_t kRawMaxFrames = 128;      // не більше kBlockFrames (перевіряється в line_path.cpp)
constexpr uint32_t kRawWaitMs = 300;         // скільки чекати знімок (потрібен хоч один оброблений блок)

// lp.cfg: вікно виміру частоти кадрів RX / TX.
constexpr uint32_t kCfgMeasureMs = 2000;
}  // namespace line_path_diag_cfg
