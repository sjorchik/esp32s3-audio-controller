#pragma once

// [USB6] fs::FS-обгортка над VFS-монтуванням USB-флешки (точка монтування usb_host_cfg::kMountPath).
// Потрібна для Audio::connecttoFS(fs::FS&, ...) у закріпленому коміті бібліотеки (розділ 11).
//
// Контракт:
//  - шляхи відносно кореня флешки, з '/' на початку ("/song.mp3", "/" — корінь); точку монтування
//    обгортка додає сама; ".." відхиляється;
//  - лише читання: режими запису відхиляються, rename / remove / mkdir / rmdir повертають false;
//  - том не змонтовано або знято під час операції: exists() → false, open() → порожній File,
//    перелічення порожнє; VFS-виклики не робляться;
//  - кожна операція тримає блокування тому лише на час одного виклику VFS (usb_msc_mount::volumeLock).

#include "config/features.h"

#if FEATURE_USB_HOST

#include "FS.h"

namespace usb_fs {

// Глобальний об'єкт fs::FS над /usb. Створюється при першому виклику. Піднімання й зняття стеку
// USB-host — справа usb_msc_mount (begin() / end()); сам об'єкт нічого не піднімає й не знімає.
fs::FS& volume();

#if USB_HOST_DIAG
// Діагностика usb.fs.* (викликається з usb_msc_mount::handleLine для рядків "usb.fs.*").
// Викликається з loopTask, як і usb.*. Не створює задач, не пише на носій.
void handleConsoleLine(const char* line);
#endif

}  // namespace usb_fs

#endif  // FEATURE_USB_HOST
