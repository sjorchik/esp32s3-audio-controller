#pragma once

// [USB13] Діагностика пам'яті для DEF-5 / DEF-9 (лише USB_HOST_DIAG = 1; у релізі модуль не збирається).
//  - знімок кучі за КЛАСАМИ (INTERNAL, INTERNAL|8BIT, DMA, PSRAM): вільно / найбільший блок / мін. вільно /
//    кількість вільних і виділених блоків / виділено байт (усе з heap_caps_get_info);
//  - хук невдалих виділень (heap_caps_register_failed_alloc_callback): розмір, caps і функція ПЕРШИХ невдалих
//    виділень — саме це покаже, ЧОМУ радіо дає «(-32512) SSL - Memory allocation failed»;
//  - друк одним рядком у форматі «[MEM] <мітка> t=… | INT … | I8 … | DMA … | PSRAM … | tasks=…».
// [DEF5a] При USB_HEAPMAP = 1 хук невдалих виділень додатково ЗАПИСУЄ (без друку й без виділень) розмір, caps, копію імені
// функції й час у кільце; друкує їх пізніше usb_heapmap зі звичайної задачі (failSeq / failCopySince / capsToText).
// Потоки: print / take можна викликати з будь-якої ЗАДАЧІ (не з ISR). Хук невдалих виділень не блокує й не
// виділяє пам'ять (esp_rom_printf, атомарні лічильники).

#include <stdint.h>

#include "config/features.h"
#if USB_HEAPMAP
#include <stddef.h>

#include "config/usb_heapmap_config.h"
#endif

#if FEATURE_USB_HOST && USB_HOST_DIAG

namespace usb_memdiag {

struct ClassInfo {
    uint32_t freeBytes;
    uint32_t largest;
    uint32_t minFree;
    uint32_t freeBlocks;
    uint32_t allocBlocks;
    uint32_t allocBytes;
};

struct Snap {
    uint32_t tMs;
    ClassInfo internal;   // MALLOC_CAP_INTERNAL
    ClassInfo internal8;  // MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    ClassInfo dma;        // MALLOC_CAP_DMA
    ClassInfo psram;      // MALLOC_CAP_SPIRAM
    uint32_t tasks;       // uxTaskGetNumberOfTasks()
};

// Знімок (без друку).
Snap take();

// Знімок + друк одним рядком. withIntegrity = true додає heap_caps_check_integrity для INTERNAL і PSRAM
// (обходить усі купи під їхніми блокуваннями: десятки мс — лише на «віхах», не в гарячих місцях).
Snap print(const char* label, bool withIntegrity = false);
void print(const char* label, const Snap& s);

// Різниця after - before за кожним класом (вільно / найбільший / виділено байт / виділено блоків), одним рядком.
void printDelta(const char* label, const Snap& before, const Snap& after);

// Реєструє хук невдалих виділень (ідемпотентно; лише ОДИН хук на систему — якщо його вже зайнято, друкує про це).
void installFailHook();

// Лічильники й останні невдалі виділення (розмір, caps, функція, час).
void printFailStats();

#if USB_HEAPMAP
// [DEF5a] Запис невдалого виділення (копія з хука). seq — порядковий номер (1, 2, …); 0 у кільці — слот не заповнено.
struct FailEntry {
    uint32_t seq;
    uint32_t size;
    uint32_t caps;
    uint32_t tMs;
    char fn[usb_heapmap_cfg::kFailFnChars];
};
// Скільки невдалих виділень зафіксовано хуком за запуск (нове значення — нові записи).
uint32_t failSeq();
// Копіює записи з seq > afterSeq (від старого до нового, не більше max; записи, перезаписані кільцем або змінені під час
// копіювання, пропускаються). Повертає кількість скопійованих. Викликати з ЗАДАЧІ.
uint32_t failCopySince(uint32_t afterSeq, FailEntry* out, uint32_t max);
// Розшифровка caps («INTERNAL|8BIT» тощо).
void capsToText(uint32_t caps, char* out, size_t cap);
#endif

}  // namespace usb_memdiag

#endif  // FEATURE_USB_HOST && USB_HOST_DIAG
