#pragma once

// [USB10] Константи браузера папок флешки (src/usb/usb_browser.*) і фонової задачі usb_nav (спільна для браузера й черги).
// Прапорець умовної компіляції FEATURE_USB_MODE — у config/features.h.
//
// Позначки джерел значень:
//   [USB3..9] — з виміряних кроків (розділ 11 MASTER SPEC);
//   [W1]      — правило проєкту про ядра / пріоритети (розділ 5);
//   ПРИПУЩЕННЯ — вибір БЕЗ вимірювання, підтверджується в hwcheck_USB10.md (таблиця T).

#include <stddef.h>
#include <stdint.h>

#include "config/usb_host_config.h"
#include "config/usb_mode_config.h"

namespace usb_browser_cfg {

// --- Фонова задача usb_nav (одна; живе лише поки режим «USB» у Ready / Playing, сама видаляється) ---------------------
// Чому окрема задача, а не usb_mode: задача usb_mode одноразова й виходить у Ready (usb_mode.cpp), а автоперехід
// потребує постійного опитування; AppController не чіпаємо. Лише ядро 0 і пріоритет 1 [W1].
constexpr int kTaskCore = 0;                    // [W1]
constexpr unsigned kTaskPriority = 1;           // [W1] «не вище мережевих»
// ПРИПУЩЕННЯ: 6144 Б = стек usb_mgr [USB2], яка теж виконує VFS / FatFS (там мін. вільно 4 048–4 284 Б). Тут: два буфери
// шляху по kPathMax (2 × 256 Б), fs::File (shared_ptr) і VFS open / readdir, Serial.printf з довгими UTF-8 іменами.
// Реальний мін. вільний стек друкує `usb.q stat`; стек Є ЛИШЕ в режимі «USB».
constexpr uint32_t kTaskStack = 6144;
constexpr const char* kTaskName = "usb_nav";
constexpr uint32_t kPollMs = usb_mode_cfg::kPollMs;  // [USB4] 20 мс: період циклу (автоперехід бачить прапорець eof за ≤ 20 мс)
constexpr uint8_t kCmdQueueLen = 8;                  // ПРИПУЩЕННЯ: команди рідкі (веб / консоль); повна черга = відмова «queue_full»

// --- Навігація ------------------------------------------------------------------------------------------------------
constexpr size_t kPathMax = usb_host_cfg::kPathMax;  // [USB6] 256 (разом з точкою монтування)
// Глибина вкладення від кореня. ПРИПУЩЕННЯ: музичні дерева «виконавець / альбом / диск» = 3–4; 8 з запасом. Справжня межа —
// довжина шляху (kPathMax), перевіряється окремо.
constexpr uint8_t kMaxDepth = 8;

// Ліміт записів у списку папки (папки + файли, що пройшли фільтр). ПРИПУЩЕННЯ: 1000 — з запасом над «≥ 500 файлів» зі
// спеки; час сортування й PSRAM (3 списки × (kMaxEntries × 12 Б + kNamePoolBytes)) малі; значення уточнюється виміром refresh.
constexpr uint16_t kMaxEntries = 1000;
// Пул імен у UTF-8 (з '\0'). ПРИПУЩЕННЯ: ≈ 65 Б на запис при ліміті записів; вичерпання пулу = теж «обрізано».
constexpr uint32_t kNamePoolBytes = 65536;

// Розширення, які показуються як файли (нечутливе до регістру ASCII). ПРИПУЩЕННЯ (рішення власника: лише .mp3).
constexpr const char* const kAudioExtensions[] = {".mp3"};
// Службові імена (нечутливо до регістру ASCII), що пропускаються разом з будь-яким імʼям, яке починається з '.'
// (у т. ч. «._файл.mp3» від macOS). Джерело: імена Windows / macOS / Android.
constexpr const char* const kSkipNames[] = {"System Volume Information", "$RECYCLE.BIN", "RECYCLER", "LOST.DIR", "FOUND.000"};

// --- Потокобезпека / планування -------------------------------------------------------------------------------------
// Очікування мьютекса списку в публічних гетерах (веб-обробники в async_tcp не блокуємо). Тримається лише на копіювання
// одного запису / полів; читання флешки під ним НЕ виконується. ПРИПУЩЕННЯ.
constexpr uint32_t kApiLockWaitMs = 20;
// Очікування блокування тому для читання покоління монтування (задача usb_nav). Найдовше утримання — одне читання
// декодера (до ≈ 150 мс, USB9: max 156 мс). ПРИПУЩЕННЯ.
constexpr uint32_t kGenLockWaitMs = 250;
// Період перевірки покоління монтування (вийняття / вставлення) задачею usb_nav. Кожна перевірка бере блокування тому
// (те саме, що й читання декодера), тож рідше, ніж kPollMs. ПРИПУЩЕННЯ: 100 мс — вийняття видно за ≤ 0,1 с.
constexpr uint32_t kMediaPollMs = 100;
// Читання каталогу: кожні kYieldEvery записів — vTaskDelay(1), щоб не голодувати usb_lib / usb_mgr (той самий пріоритет).
constexpr uint16_t kYieldEvery = 16;  // ПРИПУЩЕННЯ
// Поріг попередження «читання каталогу повільне», мс (лише для логу).
constexpr uint32_t kRefreshWarnMs = 2000;  // ПРИПУЩЕННЯ

static_assert(kMaxEntries >= 1 && kMaxEntries <= 4000, "usb_browser: sane entry limit");
static_assert(kNamePoolBytes >= 4096, "usb_browser: name pool too small");
static_assert(kMaxDepth >= 1 && kMaxDepth <= 16, "usb_browser: sane depth");
static_assert(kTaskPriority <= 2, "usb_nav task must not outrank the network tasks (async_tcp = 2)");
static_assert(kMediaPollMs >= kPollMs, "usb_browser: media poll not faster than the worker loop");
static_assert(kPathMax >= 64, "usb_browser: path buffer too small");

}  // namespace usb_browser_cfg
