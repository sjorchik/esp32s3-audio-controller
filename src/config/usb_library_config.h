// [USB22a-1] Налаштування бібліотеки USB (плоский список усіх файлів флешки; GET /api/usb/library).
// Таблиці бібліотеки — це ті самі таблиці «all» з USB20a-2 (usb_queue_all_config.h): їхні ліміти (файли / папки / пул / час скану)
// діють і тут. Окремих буферів бібліотека не виділяє.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace usb_library_cfg {

// Запит бібліотеки (прапорець з async_tcp) старіший за цей час уважається застарілим і ігнорується: так прапорець, виставлений
// просто перед виходом із режиму, не запустить скан у наступній сесії.
constexpr uint32_t kReqStaleMs = 5000;

// Мінімальний розмір буфера usb_queue::libraryPage: запис «імʼя\0папка\0» — до (255 + 1) + (255 + 1) байтів.
constexpr size_t kPageBufMin = 520;

}  // namespace usb_library_cfg
