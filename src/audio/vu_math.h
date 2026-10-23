#pragma once

// Спільна математика VU (шкала dBFS і балістика) для VuSourceDecodedPcm (радіо) та
// VuSourceLinePath (зовнішні входи). Винесена з audio/vu_source.cpp без зміни поведінки.
// [VU1] + msqToNorm(): середній квадрат (RMS) -> 0..1 тією самою шкалою; peakToNorm() і ballistics() не змінено.

#include <math.h>
#include <stdint.h>

#include "config/vu_config.h"

namespace vu_math {

// Пік int16 (0..32768) -> 0..1 за шкалою dBFS від kDbFloor до 0 дБ.
// [V1] sensDb — чутливість VU активного входу, дБ (vu_sens_cfg): ДОДАЄТЬСЯ до рівня в дБ перед відображенням
// (0 дБ показу = 1.0, kDbFloor = 0.0; надлишок вище 0 дБ обрізається нормалізацією в n). Із sensDb = 0.0f результат
// БІТОВО збігається з варіантом без параметра (x + 0.0f == x). Викликається лише з display-задачі (read()), не з
// гарячого шляху аудіо; log10f тут і так був.
inline float peakToNorm(uint32_t peak, float sensDb = 0.0f) {
    if (peak == 0) {
        return 0.0f;
    }
    float p = static_cast<float>(peak) / vu_cfg::kFullScale;
    if (p > 1.0f) {
        p = 1.0f;
    }
    const float db = 20.0f * log10f(p) + sensDb; // <= 0 без чутливості; [V1] із додатною може бути > 0
    float n = 1.0f - db / vu_cfg::kDbFloor;      // 0 дБ -> 1, kDbFloor -> 0
    if (n < 0.0f) {
        n = 0.0f;
    } else if (n > 1.0f) {
        n = 1.0f;
    }
    return n;
}

// [VU1] Середній квадрат за вікно (одиниці int16², повна шкала 32768 -> 2^30) -> 0..1 тією самою шкалою, що й
// peakToNorm(): дБ = 10*log10((RMS / повна шкала)²) + kRmsOffsetDb + sensDb. kRmsOffsetDb (+3,01 дБ) вирівнює
// синус 0 dBFS із показом за піком; sensDb (V1) додається після нього. Цифрова тиша (meanSq <= 0 або NaN) ->
// 0.0f без log10(0) / -inf. Викликається лише з display-задачі (read()), не з гарячого шляху аудіо.
inline float msqToNorm(float meanSq, float sensDb = 0.0f) {
    if (!(meanSq > 0.0f)) {
        return 0.0f;
    }
    float r = meanSq / (vu_cfg::kFullScale * vu_cfg::kFullScale);
    if (r > vu_cfg::kRmsRatioCap) {
        r = vu_cfg::kRmsRatioCap;
    }
    const float db = 10.0f * log10f(r) + vu_cfg::kRmsOffsetDb + sensDb;
    float n = 1.0f - db / vu_cfg::kDbFloor;  // 0 дБ -> 1, kDbFloor -> 0
    if (n < 0.0f) {
        n = 0.0f;
    } else if (n > 1.0f) {
        n = 1.0f;
    }
    return n;
}

// Балістика: швидка атака (вгору), повільний спад (вниз); dt-залежна.
inline float ballistics(float level, float target, float dtMs) {
    const float tau = (target > level) ? vu_cfg::kAttackMs : vu_cfg::kReleaseMs;
    float next = level + (target - level) * (dtMs / (tau + dtMs));
    if (next < 0.001f) {
        next = 0.0f;  // щоб спад завершувався рівно в 0
    }
    return next;
}

}  // namespace vu_math

