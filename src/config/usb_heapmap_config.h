#pragma once

// [DEF5a] Константи діагностики «карта внутрішньої кучі» (src/usb/usb_heapmap.*). Діють лише при USB_HEAPMAP = 1
// [DEF5c] ДОДАНО розділ «власник блока» (історія знімків циклу, дамп вмісту кандидатів, usb.hm mem): див. кінець namespace.
// (config/features.h); значення тут НЕ змінюють поведінку збірок з USB_HEAPMAP = 0.
//
// Позначки джерел значень:
//   ПРИПУЩЕННЯ — вибір планування БЕЗ вимірювання (див. hwcheck_DEF5a.md);
//   [код]      — значення виведено з констант проєкту (посилання на їхні namespace).

#include <stddef.h>
#include <stdint.h>

#include "config/usb_browser_config.h"
#include "config/usb_host_config.h"
#include "config/usb_mode_config.h"

namespace usb_heapmap_cfg {

// --- Знімок кучі (буфери в PSRAM, виділяються ОДИН раз при першому використанні) ---------------------------------
// Скільки записів (8 Б: адреса, розмір + біт «used») вміщує один знімок. ПРИПУЩЕННЯ: внутрішня куча S3 має сотні
// блоків (≈ 400 КБ, типово до ~1500 разом із вільними); 3000 — запас ×2. Перевищення -> ознака «truncated».
constexpr uint32_t kHeapMapMaxBlocks = 3000;
// Скільки задач запам'ятовує реєстр «адреса стека / TCB -> імʼя» (для міток блоків). ПРИПУЩЕННЯ.
constexpr uint32_t kTaskRegistryMax = 48;
// Скільки задач читає uxTaskGetSystemState за раз (тимчасовий масив теж у PSRAM). ПРИПУЩЕННЯ.
constexpr uint32_t kTaskScanMax = 64;

// --- Вивід (ліміт ≈ 60 рядків на цикл: 8 точок × ≈ 7 рядків) -------------------------------------------------------
// Блоки менших розмірів агрегуються (кількість + сума). За спекою — 256 Б.
constexpr uint32_t kSmallBlockBytes = 256;
// Скільки «нових» блоків ≥ kSmallBlockBytes друкувати на одній точці (решта — рядком «… ще N»).
constexpr uint32_t kDiffLinesPerPoint = 3;
// Скільки «ПОСТІЙНИХ нових» блоків друкувати при порівнянні C1(цикл n) з C1(цикл n−1) / C5 з C1 того ж циклу.
constexpr uint32_t kPersistentLines = 8;
// Скільки кандидатів зберігати для вибору найбільших (масив у PSRAM-арені).
constexpr uint32_t kCandidateMax = 96;
// Скільки найбільших вільних блоків друкувати на точці.
constexpr uint32_t kTopFreeBlocks = 3;
// C0 виконується, поки радіо грає: обхід тримає блокування кучі. Якщо обхід тривав довше — C0 далі пропускається.
constexpr uint32_t kC0WalkMaxMs = 10;
// Скільки повторних підключень радіо після виходу (C6 / C7) знімати за цикл (збій повторюється з backoff).
constexpr uint32_t kRetryPointsPerCycle = 4;

// --- usb.hm dump ---------------------------------------------------------------------------------------------------
// «dump»: вільні блоки + використані від цього розміру, не більше kDumpMaxLines рядків. «dump full»: без ліміту, used ≥ 256.
constexpr uint32_t kDumpUsedMinBytes = 1024;
constexpr uint32_t kDumpMaxLines = 120;
// Пауза кожні N рядків dump (щоб не переповнити буфер Serial). ПРИПУЩЕННЯ.
constexpr uint32_t kDumpYieldEvery = 12;

// --- Хук невдалих виділень (запис без друку) -----------------------------------------------------------------------
// Розмір кільця записів (static .bss, внутрішня RAM: kFailRing × 40 Б). ПРИПУЩЕННЯ.
constexpr uint32_t kFailRing = 32;
constexpr size_t kFailFnChars = 24;  // копія імені функції (з нуль-термінатором)

// --- usb.hm cycle ---------------------------------------------------------------------------------------------------
constexpr uint32_t kCycleMaxCount = 30;
constexpr uint32_t kCycleFileMaxSec = 120;
constexpr uint32_t kCycleDefaultFileSec = 10;
// Скільки чекати результат підключення радіо (C7) після виходу. ПРИПУЩЕННЯ: TLS-підключення + backoff 1/2/4 с.
constexpr uint32_t kCycleResultWaitMs = 30000;
// Скільки чекати старт відтворення файлу після playFile().
constexpr uint32_t kCycleFileStartWaitMs = 8000;
constexpr uint32_t kCyclePollMs = 50;
// Розширення, які шукає «перший трек» у корені тому (регістр ігнорується).
constexpr const char* const kAudioExt[] = {".mp3", ".flac", ".wav", ".aac", ".m4a", ".ogg", ".opus"};
constexpr size_t kAudioExtCount = sizeof(kAudioExt) / sizeof(kAudioExt[0]);

// --- Мітки за точним збігом розміру (лише ПІДКАЗКА, не атрибуція: власник блока — «?») -----------------------------
struct KnownSize {
    uint32_t size;
    const char* what;  // коротка назва (друкується у мітці «hint:»)
    bool isStack;      // true — розмір збігається зі стеком задачі з цією назвою
};
// Розміри блоків у купі округлюються до 4 Б; усі стеки нижче кратні 4. Значення — з config/*; назви задач — як у xTaskCreate.
constexpr KnownSize kKnownSizes[] = {
    {usb_mode_cfg::kTaskStack, "usb_mode", true},                // 4096 (збігається з usb_lib / USB MSC)
    {usb_host_cfg::kLibTaskStack, "usb_lib", true},              // 4096
    {usb_host_cfg::kMscTaskStack, "USB MSC", true},              // 4096
    {usb_host_cfg::kMgrTaskStack, "usb_mgr", true},              // 6144 (збігається з usb_nav)
    {usb_browser_cfg::kTaskStack, "usb_nav", true},              // 6144
    {usb_host_cfg::kRestartTaskStack, "ssl_restart", true},      // 5120
    {16720, "TLS-buf 16717 aligned (Г1)", false},                // ГІПОТЕЗА Г1: буфер TLS радіо (16717 -> 16720)
};
constexpr size_t kKnownSizeCount = sizeof(kKnownSizes) / sizeof(kKnownSizes[0]);

// --- [DEF5c] Власник блока, що розрізає вільну «діру»: історія знімків циклу, дамп вмісту, usb.hm mem --------------------
// Скільки знімків поточного циклу зберігати для обчислення first_seen: C1 … C6 (індекс = номер точки − 1). Кожен знімок —
// kHeapMapMaxBlocks × 8 Б у PSRAM-арені (6 × 3000 × 8 = 144 КБ). ПРИПУЩЕННЯ: PSRAM вистачає (≈ 6,7 МБ вільно).
constexpr uint32_t kHistoryPoints = 6;
// Скільки кандидатів вибирати на C5 (INSIDE першими, далі за розміром спадання). ПРИПУЩЕННЯ (за промптом).
constexpr uint32_t kDumpMaxBlocks = 6;
// Для скількох перших кандидатів друкувати ПОВНИЙ вміст (hex + класифікація). Решта — лише рядок `blk` (вміст — `usb.hm mem`).
// ПРИПУЩЕННЯ: 3 вкладаються в ≈ 25 рядків на цикл (1 підсумок + 6 `blk` + 3 × 5 + 1 рядок C6 = 23). Для всіх 6 — поставити 6
// (≈ 38 рядків на цикл).
constexpr uint32_t kDumpDetailBlocks = 3;
// Скільки байтів з початку блока друкувати hex-словами, і скільки з кінця (окремим рядком, лише якщо блок довший за голову).
constexpr uint32_t kDumpHeadBytes = 96;
constexpr uint32_t kDumpTailBytes = 16;
// 32-бітних слів у рядку hex-дампу (8 слів = 32 Б).
constexpr uint32_t kDumpWordsPerLine = 8;
// usb.hm mem <адреса> [байт]: типово / максимум байтів (понад максимум — обрізається до максимуму).
constexpr uint32_t kMemDefaultBytes = 96;
constexpr uint32_t kMemMaxBytes = 256;
// Текстові послідовності: мінімальна довжина (друковані ASCII 0x20…0x7E), скільки показувати на блок, скільки символів на рядок.
constexpr uint32_t kStrMinLen = 4;
constexpr uint32_t kStrMaxCount = 4;
constexpr uint32_t kStrMaxChars = 24;
// Скільки областей внутрішньої кучі (start / end із heap_caps_walk) запамʼятовувати для перевірки адреси в `usb.hm mem`.
constexpr uint32_t kRegionMax = 8;
// Буфер складання довгих рядків (класифікація, C6) — у PSRAM-арені.
constexpr uint32_t kTextBufBytes = 512;

// Класи слів у дампі (діапазони адрес ESP32-S3). ПРИПУЩЕННЯ: rodata прошивки менший за 16 МБ, тому 0x3D000000…0x3E000000
// відносимо до `ptr:` (у промпті діапазон rodata [0x3C000000, 0x3E000000) перекривається з «0x3Dxxxxxx — ptr»; PSRAM на S3
// відображається в тому ж вікні 0x3C…0x3E, тож `rodata:` у молодших адресах може містити й вказівники в PSRAM).
constexpr uint32_t kCodeLow = 0x42000000u;    // flash-код (addr2line)
constexpr uint32_t kCodeHigh = 0x44000000u;
constexpr uint32_t kIramLow = 0x40370000u;    // код в IRAM (ISR / IRAM_ATTR; addr2line теж підходить) — додатковий клас `iram:`
constexpr uint32_t kIramHigh = 0x403E0000u;
constexpr uint32_t kRodataLow = 0x3C000000u;  // flash rodata
constexpr uint32_t kRodataHigh = 0x3D000000u;
constexpr uint32_t kPtrLowA = 0x3D000000u;    // вказівники: 0x3Dxxxxxx …
constexpr uint32_t kPtrHighA = 0x3E000000u;
constexpr uint32_t kPtrLowB = 0x3F000000u;    // … і 0x3Fxxxxxx (у тому числі внутрішня DRAM 0x3FC8…0x3FCF)
constexpr uint32_t kPtrHighB = 0x40000000u;

static_assert(kHeapMapMaxBlocks >= 256 && kHeapMapMaxBlocks <= 20000, "usb_heapmap: sane block limit");
static_assert(kTaskScanMax >= 16 && kTaskRegistryMax >= 16, "usb_heapmap: sane task tables");
static_assert(kCycleMaxCount >= 1 && kCycleMaxCount <= 100, "usb_heapmap: sane cycle limit");
static_assert(kHistoryPoints == 6, "usb_heapmap: history holds C1..C6");
static_assert(kDumpMaxBlocks >= 1 && kDumpMaxBlocks <= 16, "usb_heapmap: sane candidate limit");
static_assert(kDumpDetailBlocks <= kDumpMaxBlocks, "usb_heapmap: detail blocks <= candidates");
static_assert(kDumpHeadBytes % 4 == 0 && kDumpTailBytes % 4 == 0 && kDumpHeadBytes >= 4, "usb_heapmap: dump sizes are word multiples");
static_assert(kMemMaxBytes % 4 == 0 && kMemMaxBytes >= kDumpHeadBytes + kDumpTailBytes, "usb_heapmap: mem buffer holds head + tail");
static_assert(kMemDefaultBytes <= kMemMaxBytes && kMemDefaultBytes % 4 == 0, "usb_heapmap: sane mem default");
static_assert(kDumpWordsPerLine >= 1 && kDumpWordsPerLine <= 8, "usb_heapmap: hex line fits the line buffer");
static_assert(kRegionMax >= 2 && kTextBufBytes >= 256, "usb_heapmap: sane region / text buffers");

}  // namespace usb_heapmap_cfg

