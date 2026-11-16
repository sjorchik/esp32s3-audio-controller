#pragma once

// [USB20a-2] Константи черги «уся флешка» (src/usb/usb_queue.cpp, опція scope = "all"; див. config/usb_queue_scope_config.h).
// Позначки: ПРИПУЩЕННЯ — значення без вимірювання (уточнює hwcheck_USB20a2.md).

#include <stddef.h>
#include <stdint.h>

namespace usb_queue_all_cfg {

// Скільки файлів вміщає черга «all»; понад ліміт решта відкидається, queue.truncated = true. Індекси треків — uint16_t.
constexpr uint16_t kQueueAllMax = 3000;
// Скільки папок (разом з коренем) запамʼятовує скан. Папка без файлів теж займає запис. ПРИПУЩЕННЯ: 1024 з запасом для
// бібліотеки на ≈ 3 000 файлів.
constexpr uint16_t kQueueAllDirMax = 1024;
// Спільний пул імен файлів і папок (PSRAM). ПРИПУЩЕННЯ: ≈ 65 Б на імʼя (як kNamePoolBytes / kMaxEntries у браузері) × 3 000 +
// запас під імена папок; імена кирилицею — 2 Б на літеру, тож пул може вичерпатись раніше за kQueueAllMax (тоді truncated).
constexpr uint32_t kQueueAllPoolBytes = 245760;  // 240 КБ

// Скан іде кроками в tick() задачі usb_nav: за один такт — не довше цього часу (плюс завершення читання поточної папки), далі
// цикл віддає керування (vTaskDelay(kPollMs) і обробка нових команд). ПРИПУЩЕННЯ: 50 мс.
constexpr uint32_t kScanSliceMs = 50;
// Аварійна межа тривалості всього скану: далі скан зупиняється, queue.truncated = true. ПРИПУЩЕННЯ: 60 с (див. оцінку квадратичної
// ціни open(path) у широких каталогах у hwcheck_USB20a2.md).
constexpr uint32_t kScanMaxMs = 60000;
// Скан починається, коли трек X ЗАГРАВ (фаза Running), щоб читання флешки не уповільнювало його відкриття (вимір USB20a-2:
// без цього accept->playing ≈ 1450 мс проти ≈ 870 мс у режимі folder). Якщо X не заграв за цей час після play — скан
// починається все одно (збій відкриття X сам скасує скан через failAndSkip). ПРИПУЩЕННЯ: 3 с (старт треку ≈ 0,9 с).
constexpr uint32_t kScanStartGraceMs = 3000;
// Скільки тактів поспіль повторювати читання папки, поки том зайнятий декодером (ReadResult::VolumeBusy), перш ніж пропустити
// папку (truncated). ПРИПУЩЕННЯ: 40 × (kPollMs + читання) ≈ 2–4 с.
constexpr uint8_t kScanBusyRetryMax = 40;

static_assert(kQueueAllMax >= 1000 && kQueueAllMax <= 60000, "usb_queue_all: sane file limit (uint16 indices)");
static_assert(kQueueAllDirMax >= 2 && kQueueAllDirMax < 0xFFFF, "usb_queue_all: sane dir limit (0xFFFF = no node)");
static_assert(kQueueAllPoolBytes >= 65536, "usb_queue_all: name pool too small");
static_assert(kScanSliceMs >= 1 && kScanSliceMs <= 500, "usb_queue_all: sane scan slice");
static_assert(kScanMaxMs >= 1000, "usb_queue_all: scan time cap too small");
static_assert(kScanStartGraceMs >= 500 && kScanStartGraceMs <= 10000, "usb_queue_all: sane scan start grace");

}  // namespace usb_queue_all_cfg
