#pragma once

// Спільна математика VU (шкала dBFS і балістика) для VuSourceDecodedPcm (радіо) та
// VuSourceLinePath (зовнішні входи). Винесена з audio/vu_source.cpp без зміни поведінки.

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
