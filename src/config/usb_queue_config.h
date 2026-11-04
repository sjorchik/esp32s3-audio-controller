#pragma once

// [USB10] Константи черги відтворення (src/usb/usb_queue.*): автоперехід, пропуск пошкоджених файлів, історія перемішування.
// Позначки: [USB7] — виміряно / з файлового плеєра; ПРИПУЩЕННЯ — без вимірювання (уточнює hwcheck_USB10.md).

#include <stddef.h>
#include <stdint.h>

#include "config/file_player_config.h"

namespace usb_queue_cfg {

// Скільки файлів ПОСПІЛЬ можна пропустити (не відкрились / збій читання), перш ніж зупинити чергу з причиною
// «too_many_skips». ПРИПУЩЕННЯ: 5 — флешка з кількома битими файлами проходиться, а черга з самих битих (повтор усього!) не
// крутиться вічно; найгірший час = 5 × kStartTimeoutMs [USB7] (5 с), типовий збій відкриття — одиниці мс.
constexpr uint8_t kMaxConsecutiveSkips = 5;

// Після playFile() плеєр підхоплює запит асинхронно; «Error» у цей проміжок — ЗАЛИШОК попереднього збою, а не відповідь на
// новий файл. Помилка трактується як збій нового файлу лише після цієї паузи. ПРИПУЩЕННЯ: час підхоплення задачею плеєра
// не виміряно (лог `[USBQ] ... accept->active` у hwcheck_USB10.md дасть число).
constexpr uint32_t kStartGraceMs = 250;
// Аварійний ліміт очікування Playing після прийнятого запиту (на випадок, коли плеєр завис у Buffering): власний
// «start timeout» плеєра [USB7] kStartTimeoutMs + запас.
constexpr uint32_t kStartGuardMs = file_player_cfg::kStartTimeoutMs + 1500;
// «Зовнішня зупинка» (Idle без прапорця eof і без fileActive) визнається лише якщо тримається стільки мс: прапорець eof
// піднімається ПІСЛЯ переходу в Idle (USB7), тож без витримки вона плутається з природним кінцем. ПРИПУЩЕННЯ.
constexpr uint32_t kExternalStopDebounceMs = 400;
// Період перевірки, що плеєр грає саме наш файл (якщо його підмінили `usb.play`, черга не втручається).
constexpr uint32_t kPathCheckMs = 250;  // ПРИПУЩЕННЯ

// Глибина історії «prev» і стеку «next після prev» у режимі перемішування (по записі = 2 Б). ПРИПУЩЕННЯ: 32.
constexpr uint8_t kHistoryMax = 32;

// Скільки елементів порядку друкувати в логу при побудові проходу (щоб у hwcheck порівняти два проходи).
constexpr uint16_t kOrderLogMax = 64;

// Очікування результату команди в діагностичній консолі (блокує loopTask лише в USB_HOST_DIAG).
constexpr uint32_t kConsoleWaitMs = 8000;  // ПРИПУЩЕННЯ: refresh папки зі ≥ 500 файлів + запас

static_assert(kMaxConsecutiveSkips >= 1 && kMaxConsecutiveSkips <= 50, "usb_queue: sane skip limit");
static_assert(kStartGuardMs > kStartGraceMs, "usb_queue: guard must exceed grace");
static_assert(kHistoryMax >= 2 && kHistoryMax <= 200, "usb_queue: sane history");

}  // namespace usb_queue_cfg
