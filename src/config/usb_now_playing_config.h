#pragma once

// [USB17a] Константи даних «зараз грає» для екрана режиму «USB» (src/usb/usb_now_playing.*, AppStateData::usbNow).
// Малює ці дані крок USB17b; тут лише бюджети буферів і прапорець діагностичного логу.

#include <stddef.h>

// Діагностичний лог [USBNP] (Serial, англійською): рядок при першому знімку та при КОЖНІЙ зміні опублікованих даних
// (у т. ч. секунди часу, тож ≈ 1 рядок / с, поки грає трек). 1 = ТИМЧАСОВО на час hwcheck_USB17a; у релізі 0
// (при 0 код логу компілюється геть).
#define USB_NOWPLAYING_TRACE 1

namespace usb_now_playing_cfg {

// Розмір буфера тексту (title / artist) разом з '\0', UTF-8. ПРИПУЩЕННЯ: 64 Б = file_player_cfg::kTagMaxBytes (збіг
// перевіряє static_assert у usb_now_playing.cpp); title без тегу = імʼя файла без розширення, усічене до цього ж розміру.
constexpr size_t kTextMax = 64;

// Мінімальний інтервал між рядками «не вдалося взяти мʼютекс черги» в логу, мс (лише USB_NOWPLAYING_TRACE).
constexpr unsigned long kLockFailLogMs = 5000;

static_assert(kTextMax >= 8 && kTextMax % 4 == 0, "kTextMax: sane size, multiple of 4 (no padding in UsbNowPlaying)");

}  // namespace usb_now_playing_cfg
