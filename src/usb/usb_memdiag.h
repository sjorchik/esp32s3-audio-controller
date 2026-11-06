#pragma once

// [USB13] Діагностика пам'яті для DEF-5 / DEF-9 (лише USB_HOST_DIAG = 1; у релізі модуль не збирається).
//  - знімок кучі за КЛАСАМИ (INTERNAL, INTERNAL|8BIT, DMA, PSRAM): вільно / найбільший блок / мін. вільно /
//    кількість вільних і виділених блоків / виділено байт (усе з heap_caps_get_info);
//  - хук невдалих виділень (heap_caps_register_failed_alloc_callback): розмір, caps і функція ПЕРШИХ невдалих
//    виділень — саме це покаже, ЧОМУ радіо дає «(-32512) SSL - Memory allocation failed»;
//  - друк одним рядком у форматі «[MEM] <мітка> t=… | INT … | I8 … | DMA … | PSRAM … | tasks=…».
// Потоки: print / take можна викликати з будь-якої ЗАДАЧІ (не з ISR). Хук невдалих виділень не блокує й не
// виділяє пам'ять (esp_rom_printf, атомарні лічильники).

#include <stdint.h>

#include "config/features.h"

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

}  // namespace usb_memdiag

#endif  // FEATURE_USB_HOST && USB_HOST_DIAG
