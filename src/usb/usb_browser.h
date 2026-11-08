#pragma once

// [USB10] Браузер папок USB-флешки (крок g2a, лише бекенд). Список поточної папки: СПОЧАТКУ ПАПКИ, ПОТІМ ФАЙЛИ, нечутливе до
// регістру впорядкування; вхід / вихід у підкаталоги. Працює лише в режимі «USB» (usb_mode Ready / Playing) і лише поки є
// змонтований том.
//
// Потоки. Читання каталогу й сортування виконує ОДНА фонова задача usb_nav (ядро 0, пріоритет 1, config/usb_browser_config.h),
// яка стартує за першим запитом і видаляється при виході з режиму. Публічні функції НЕ блокують (можна з async_tcp, UI,
// консолі): запити (request*) лише ставлять команду в чергу й повертають reqId; гетери копіюють дані під коротким
// мьютексом (kApiLockWaitMs, при таймауті — false); читання флешки під мьютексом не виконується. Новий список будується у
// «чорновому» буфері й підміняється атомарно, тож читач бачить або старий, або новий список, ніколи — напівзаповнений.
//
// Версія списку (Status::version) змінюється при кожній публікації та інвалідації; запити enterDir / playIndex приймають
// expectVersion (0 — не перевіряти): якщо список змінився, команда відхиляється з причиною «stale».
// Після вийняття / перемонтування флешки список недійсний (valid = false, count = 0); після повторної вставки задача сама
// перечитує КОРІНЬ (стару позицію не відновлює).
//
// Результат команди: Status::lastReqId / lastOk / lastWhy (ЛИШЕ остання завершена команда; команди виконуються по черзі).
//
// [USB14] ДОДАНО requestGoto(): перехід у папку за ПОВНИМ шляхом (для відновлення останньої папки при вході в режим). Решта
// API без змін.
//
// [USB15] ДОДАНО (лише нові функції / типи; наявні сигнатури й поля Status НЕ змінено):
//  - result(reqId): результат команди за її номером з кільця останніх web_api_usb_cfg::kResultRingLen (16) результатів. Одного
//    «останнього результату» (Status::lastReqId) недостатньо, коли команди ставлять кілька клієнтів. Status::lastReqId / lastOk /
//    lastWhy лишились і збігаються з найновішим записом кільця.
//  - readPage(): сторінка списку папки ОДНИМ утриманням мьютекса (узгоджені версія й імена; для веб-API GET /api/usb/list).
//
// Заголовок не підключає заголовки IDF, AppController, AudioPlayer.

#include <stddef.h>
#include <stdint.h>

#include "config/features.h"

#if FEATURE_USB_MODE

namespace usb_browser {

struct Status {
    bool active;       // задача usb_nav працює й буфери виділено
    bool valid;        // список поточної папки дійсний
    bool truncated;    // список обрізано (kMaxEntries або пул імен)
    uint32_t version;
    uint16_t count;    // записів (папки + файли)
    uint16_t dirs;     // з них папок (індекси 0 .. dirs-1)
    uint8_t depth;     // 0 — корінь
    uint16_t skippedHidden;  // службові / «.»-імена
    uint16_t skippedOther;   // файли без дозволеного розширення
    uint16_t skippedLong;    // шлях задовгий (kPathMax)
    uint32_t readMs;   // тривалість останнього читання каталогу
    uint8_t pending;   // команд у черзі + виконується
    uint32_t lastReqId;
    bool lastOk;
    const char* lastWhy;    // причина відмови останньої команди (статичний рядок) або nullptr
    const char* lastError;  // остання помилка задачі / читання (статичний рядок) або nullptr
};

struct EntryInfo {
    bool isDir;
    uint32_t size;  // ЗАВЖДИ 0 на цьому кроці: розмір потребує open() кожного файлу (O(n²) на FAT, вимір USB10: 1000 записів = 35 с)
};

// Знімок стану. false — мьютекс зайнятий довше kApiLockWaitMs (out не змінено). Якщо задача не працює — true, active = false.
bool status(Status* out);

// Копіює шлях поточної папки ("/" — корінь) у out. false — немає активного дійсного списку, малий буфер або мьютекс зайнятий.
bool cwd(char* out, size_t cap);

// Копіює запис index (імʼя UTF-8, обрізання по межі символу НЕ робиться: false при малому буфері) і метадані.
// false — індекс поза межами / список недійсний / малий буфер / мьютекс зайнятий.
bool entry(uint16_t index, char* nameOut, size_t nameCap, EntryInfo* info);

// Запити (неблокуючі). true — команду прийнято (reqId — її номер; завершення дивись Status::lastReqId). false — відмова,
// *why (якщо не nullptr): «mode_not_ready», «task_create_failed», «queue_full».
bool requestRefresh(uint32_t* reqId = nullptr, const char** why = nullptr);
// Вхід у папку з номером index (має бути папкою); expectVersion — версія списку, з якого взято index (0 — не перевіряти).
bool requestEnterDir(uint16_t index, uint32_t expectVersion = 0, uint32_t* reqId = nullptr, const char** why = nullptr);
// Вихід на рівень вище (з кореня — відмова «at_root» у lastWhy).
bool requestUp(uint32_t* reqId = nullptr, const char** why = nullptr);
// [USB14] ДОДАНО: перехід у папку за повним шляхом від кореня флешки («/», «/Music/Rock»; без «.» / «..» / порожніх компонентів;
// кінцевий «/» ігнорується). Працює і без дійсного списку. Після успіху поточний список — вміст цієї папки; після відмови
// (lastOk = false, lastWhy: «open_failed» — папки нема, «not_dir», «media_changed», …) СПИСОК НЕ ЗМІНЮЄТЬСЯ. false — запит не
// прийнято, *why: «bad_path», «too_deep», «path_too_long», «busy», «mode_not_ready», «task_create_failed», «queue_full».
// Реалізація: окремої команди в usb_nav_internal.h немає — шлях кладеться в статичний буфер, а виконує його команда Refresh
// (перша з черги, що знайде «відкладений» шлях).
bool requestGoto(const char* path, uint32_t* reqId = nullptr, const char** why = nullptr);

// [USB15] ДОДАНО: результат команди за reqId.
//  Done    — команда завершена; ok / why дійсні (why — статичний рядок або nullptr; як Status::lastWhy);
//  Pending — номер видано, команда ще в черзі або виконується;
//  Unknown — номер не видавався, застарів (виштовхнутий з кільця) або команда втрачена (задача usb_nav зупинилась до її виконання).
struct ReqResult {
    enum class State : uint8_t { Unknown, Pending, Done };
    State state;
    bool ok;
    const char* why;
};
// false — мьютекс зайнятий довше kApiLockWaitMs (out не змінено). Неблокуюча.
bool result(uint32_t reqId, ReqResult* out);

// [USB15] ДОДАНО: сторінка списку поточної папки. Записи від offset (до maxItems штук, поки вміщаються в buf) копіюються в
// buf у упакованому вигляді: для кожного запису байт 0 / 1 (1 — папка), далі імʼя UTF-8 з '\0'; імена НЕ обрізаються (запис, що
// не вміщається, і всі наступні лишаються на наступну сторінку). cwdOut (якщо не nullptr) — шлях папки. Усе під одним
// утриманням мьютекса (kApiLockWaitMs), читання флешки не виконується.
//  true  — out заповнено (out->valid = false: списку нема, записів нема). Якщо expectVersion != 0 і версія списку інша —
//          out->stale = true, записи не копіюються (список змінився між сторінками).
//  false — мьютекс зайнятий, buf замалий (< 258 Б) або null.
// out->returned — скільки записів скопійовано; наступна сторінка починається з offset + returned.
struct PageInfo {
    bool valid;
    bool stale;
    bool truncated;
    uint32_t version;
    uint16_t count;
    uint16_t dirs;
    uint8_t depth;
    uint16_t returned;
};
bool readPage(uint16_t offset, uint16_t maxItems, uint32_t expectVersion, char* buf, size_t cap, char* cwdOut, size_t cwdCap,
              PageInfo* out);

#if USB_HOST_DIAG
// Консоль: usb.dir [сторінка | refresh] | usb.cd <індекс | .. | /шлях> | usb.pwd. Викликається з loopTask; БЛОКУЄ її до завершення
// команди (≤ usb_queue_cfg::kConsoleWaitMs), як usb.life / usb.mode cycle.
bool isConsoleCommand(const char* line);
void handleConsoleLine(const char* line);
#endif

}  // namespace usb_browser

#endif  // FEATURE_USB_MODE
