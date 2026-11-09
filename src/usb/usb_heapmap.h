#pragma once

// [DEF5a] Карта внутрішньої кучі для DEF-5 (ЛИШЕ діагностика; USB_HEAPMAP = 1, config/features.h).
// Мета: з'ясувати, ЯКИЙ блок внутрішньої RAM переживає цикл «радіо -> режим USB (файл) -> радіо» і розрізає великий
// вільний блок, та які саме виділення (розмір, caps, функція) падають при збої https-підключення радіо.
//
// Знімок = обхід heap_caps_walk(MALLOC_CAP_INTERNAL, …) у буфер у PSRAM (8 Б на блок). Під час обходу НІЧОГО не
// друкується й не виділяється (callback виконується під блокуванням кучі). Друк — після обходу, у звичайній задачі.
//
// Контрольні точки циклу (макрос HEAPMAP_POINT(tag); порожній, якщо USB_HEAPMAP = 0):
//   C0 радіо грає, перед входом            C4 файл зупинено, перед виходом (перед end())
//   C1 радіо зупинено, перед begin()       C5 після end() (host знято)
//   C2 змонтовано (enter: ready)           C6 безпосередньо перед connecttohost радіо (лише після C5 того ж циклу)
//   C3 файл грає (початок виходу)          C7 результат підключення радіо (HEAPMAP_CONNECT_RESULT(ok))
// Потоки: точки викликаються із задач usb_mode (C0 – C5) і плеєра (C6, C7); виклики серіалізує мʼютекс модуля.
// Консоль (диспетчеризація — usb_play_diag): usb.hm stat | snap <tag> | diff | dump [full] | mem <hex-адреса> [байт] | auto on|off |
// cycle <N> [<сек>].
// [CLEAN1] `cycle` імітує реальний сценарій: чекає автостарт відновлення (запасно — перший файл у корені), відлік <сек> (типово 10;
// 0 = цикл без файла) — від першого Playing; підсумок циклу містить src / play_ms / tracks_played. Публічний API не змінено.
// [DEF5c] ДОДАНО: на C5 — кандидати «власника блока» (рядки `[HM]   blk …`, hex-дамп, класифікація слів), на C6 — рядок
// `still-allocated`, консоль `usb.hm mem <hex-адреса> [байт <= 256]`. Публічний API (point / connectResult / handleConsoleLine)
// і макроси HEAPMAP_POINT / HEAPMAP_CONNECT_RESULT НЕ змінено.

#include <stdint.h>

#include "config/features.h"

#if USB_HEAPMAP

namespace usb_heapmap {

enum class Point : uint8_t { C0, C1, C2, C3, C4, C5, C6, C7 };

// Знімає контрольну точку (якщо `usb.hm auto on`), друкує стан і діф до попередньої точки.
void point(Point p);

// C7: результат connecttohost() радіо. Діє лише після C6 (мс — від кінця C6). При ok = false друкує блок «FAIL».
void connectResult(bool ok);

// Консоль: рядок починається зі слова «usb.hm». Викликається з loopTask (usb_play_diag); `cycle` блокує його.
void handleConsoleLine(const char* line);

}  // namespace usb_heapmap

#define HEAPMAP_POINT(tag) ::usb_heapmap::point(::usb_heapmap::Point::tag)
#define HEAPMAP_CONNECT_RESULT(ok) ::usb_heapmap::connectResult(ok)

#else  // !USB_HEAPMAP: нуль коду

#define HEAPMAP_POINT(tag) \
    do {                   \
    } while (0)
#define HEAPMAP_CONNECT_RESULT(ok) \
    do {                           \
    } while (0)

#endif  // USB_HEAPMAP


