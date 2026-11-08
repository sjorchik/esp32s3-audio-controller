#pragma once

// [USB15] Константи веб-API плеєра USB (src/net/web_api_usb.*) і кільця результатів команд usb_nav (src/usb/usb_browser.cpp).
// Існуючі config/*.h НЕ змінювались. Прапорець умовної компіляції — FEATURE_USB_MODE (config/features.h); константи нижче від
// нього не залежать, тож файл безпечно підключати завжди.
//
// Позначки: [USB10] / [W1] — значення з наявних модулів; ПРИПУЩЕННЯ — вибір без вимірювання, підтверджується в hwcheck_USB15.md.

#include <stddef.h>
#include <stdint.h>

namespace web_api_usb_cfg {

// --- Кільце результатів команд за reqId (usb_browser.cpp, finish()) -----------------------------------------------------
// Команди usb_nav виконуються по черзі й завершуються в порядку reqId. Одночасно «в польоті» не більше
// usb_browser_cfg::kCmdQueueLen (8) у черзі + 1 виконується = 9. Кільце 16 = запас у 7 результатів, тож клієнт, який
// опитує ≈ 2 Гц (500 мс) і ставить до 8 команд поспіль, не втрачає жодного результату, навіть якщо другий клієнт
// паралельно додає свої. Запис = 12 Б (id 4, ok 1, why 4 + вирівнювання), усього ≈ 192 Б статичної RAM. ПРИПУЩЕННЯ.
constexpr uint8_t kResultRingLen = 16;

// --- GET /api/usb/list --------------------------------------------------------------------------------------------------
// Кількість записів за замовчуванням і максимум на сторінку. Обмежувальний чинник — байти відповіді, а не кількість: див.
// kListJsonBudgetBytes. 40 записів із типовими іменами (≈ 30 байт) ≈ 2,6 КБ JSON; 100 — стеля, щоб один запит не тримав
// мьютекс списку й задачу async_tcp довше за потрібне. ПРИПУЩЕННЯ.
constexpr uint16_t kListDefaultLimit = 40;
constexpr uint16_t kListMaxLimit = 100;

// Тимчасовий буфер упакованої сторінки (імена + прапорець папки; PSRAM, виділяється на час одного запиту й звільняється).
// Одне імʼя ≤ 255 байт + службові 2 → вміщується завжди (static_assert нижче).
constexpr size_t kListRawBufBytes = 6144;

// Бюджет байтів ЗАПИСІВ у JSON-відповіді (без шапки ≈ 150 Б). Підсумок ≤ ≈ 4,3 КБ — удвічі менше за стелю 8 КБ зі спеки:
// внутрішня RAM тонка (min_free ≈ 21,7 КБ з відкритою сторінкою «Аудіо»), а AsyncResponseStream тримає відповідь у купі
// до відправки. Не влізло — зменшується кількість записів і віддається nextOffset (імена НЕ обрізаються). ПРИПУЩЕННЯ.
constexpr size_t kListJsonBudgetBytes = 4096;
// Службові символи одного запису {"i":NNNN,"name":"…","dir":false}, з комою (рахується для бюджету; точно 33 + запас).
constexpr size_t kListItemOverheadBytes = 36;

// --- POST /api/usb/seek -------------------------------------------------------------------------------------------------
// Модуль відносного зсуву, с. 3600 = година: достатньо для довгих треків, а помилковий «10^9» відсікається.
constexpr int32_t kSeekMaxAbsSec = 3600;

static_assert(kResultRingLen >= 9 + 1, "result ring must cover queue (8) + in progress (1)");
static_assert(kListDefaultLimit >= 1 && kListDefaultLimit <= kListMaxLimit, "list limits");
static_assert(kListRawBufBytes >= 2 * 258, "raw buffer must hold at least two maximal records");
static_assert(kListJsonBudgetBytes >= 256 * 6 + 64, "JSON budget must fit one worst-case escaped name");

}  // namespace web_api_usb_cfg
