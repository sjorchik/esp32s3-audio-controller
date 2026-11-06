#pragma once

// [USB13] ДОДАНО closeAllForUnmountLocked() і handleStats(): усі відкриті файли / каталоги обгортки закриваються
// ДО розмонтування VFS (DEF-9: раніше при вийманні під час файла дескриптор осиротів і лишав ≈ 4,7–5,1 КБ RAM).
// [USB7] ДОДАНО readStats() / resetReadStats() (лічильники читань; лише USB_HOST_DIAG). Решта — без змін.
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

// [USB13] Результат закриття дескрипторів перед розмонтуванням.
struct UnmountClosed {
    uint16_t files;     // закрито файлів
    uint16_t dirs;      // закрито каталогів
    uint16_t deferred;  // закрито раніше ВІДКЛАДЕНИХ закриттів (див. kFsDeferredMax)
    uint16_t leaked;    // зроблено навмисних витоків (відкладене закриття зі старого монтування: закрити небезпечно)
};

// [USB13] Закриває ВСІ відкриті файли й каталоги обгортки (fclose / closedir → f_close / f_closedir, поки том
// ще зареєстровано у VFS і змонтовано у FatFs). Для файла лише на читання f_close не звертається до диска
// (немає FA_MODIFIED), тому працює й після фізичного вийняття носія.
// ВИКЛИКАТИ ЛИШЕ з usb_msc_mount::unmountLocked(): під volumeLock (s_ioMutex), ПІСЛЯ скидання стану тому
// й ДО msc_host_vfs_unregister(). closingGen — покоління монтування, яке знімається (0 — нічого не змонтовано).
// Після виклику відкриті fs::File лишаються дійсними обʼєктами, але «порожніми»: читання повертає 0, seek / size —
// false / 0, повторне close() нешкідливе.
UnmountClosed closeAllForUnmountLocked(uint32_t closingGen);

// [USB13] Лічильники дескрипторів (атомарні; будь-яка задача).
struct HandleStats {
    uint32_t open;              // відкрито зараз (обгорток у реєстрі)
    uint32_t closedAtUnmount;   // усього закрито в closeAllForUnmountLocked (файли + каталоги) з завантаження
    uint32_t deferredTotal;     // скільки закриттів було відкладено (таймаут блокування тому)
    uint32_t leakedTotal;       // скільки навмисних витоків (має лишатись 0)
};
HandleStats handleStats();

#if USB_HOST_DIAG
// [USB7] Лічильники читань обгортки (UsbFileImpl::read) з моменту останнього resetReadStats(): кількість викликів,
// максимальна тривалість одного (мкс, разом з очікуванням блокування тому), скільки було >= usb_host_cfg::kFsSlowReadUs,
// сума прочитаних байт (32 біти, обертається). Рахує УСІ читання через обгортку (і з usb.fs.*, і декодера).
// Будь-яка задача; значення читаються без блокування (атомарні лічильники).
struct ReadStats {
    uint32_t count;
    uint32_t maxUs;
    uint32_t slowCount;
    uint32_t bytes;
};
ReadStats readStats();
void resetReadStats();

// Діагностика usb.fs.* (викликається з usb_msc_mount::handleLine для рядків "usb.fs.*").
// Викликається з loopTask, як і usb.*. Не створює задач, не пише на носій.
void handleConsoleLine(const char* line);
#endif

}  // namespace usb_fs

#endif  // FEATURE_USB_HOST
