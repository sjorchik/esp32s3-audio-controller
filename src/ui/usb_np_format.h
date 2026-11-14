#pragma once

// [USB17b] Чиста (без Arduino / LovyanGFX) логіка екрана «зараз грає»: формат часу й ширина заливки смуги прогресу.
// Окремо від screens.cpp, щоб перевірятись на ПК.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace usb_np_format {

// "m:ss" без нуля попереду у хвилинах (0:07, 12:03); хвилини обмежено maxMinutes (довше -> maxMinutes:59).
inline void formatClock(uint32_t sec, uint32_t maxMinutes, char* out, size_t cap) {
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    if (m > maxMinutes) { m = maxMinutes; s = 59; }
    snprintf(out, cap, "%lu:%02lu", static_cast<unsigned long>(m), static_cast<unsigned long>(s));
}

// "m:ss / m:ss"; тривалість невідома (durSec == 0) -> "m:ss / --:--".
inline void formatTime(uint32_t posSec, uint32_t durSec, uint32_t maxMinutes, char* out, size_t cap) {
    char a[16];
    formatClock(posSec, maxMinutes, a, sizeof(a));
    if (durSec == 0) {
        snprintf(out, cap, "%s / --:--", a);
        return;
    }
    char b[16];
    formatClock(durSec, maxMinutes, b, sizeof(b));
    snprintf(out, cap, "%s / %s", a, b);
}

// Ширина заливки смуги в пікселях [0..innerW]; durSec == 0 -> 0; posSec > durSec -> повна.
inline int32_t barFillPx(uint32_t posSec, uint32_t durSec, int32_t innerW) {
    if (durSec == 0 || innerW <= 0) return 0;
    if (posSec >= durSec) return innerW;
    return static_cast<int32_t>((static_cast<uint64_t>(posSec) * static_cast<uint64_t>(innerW)) / durSec);
}

}  // namespace usb_np_format
