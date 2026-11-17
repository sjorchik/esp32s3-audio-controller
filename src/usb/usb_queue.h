#pragma once

// [USB10] Черга відтворення (крок g2a, лише бекенд): файли ПОТОЧНОЇ папки браузера (без рекурсії), на якій запущено
// відтворення; автоперехід на наступний трек за прапорцем AudioPlayer::takeFileEnded(); послідовний режим і перемішування;
// повтор усього; пропуск файлів, що не відкрились.
//
// Правила:
//  - Черга = знімок файлів папки на момент playIndex (cd після цього її не змінює). Порядок = порядок списку браузера.
//  - Послідовно: по порядку списку. Кінець черги при автопереході: стоп, якщо немає повтору (reason «end_of_queue»),
//    інакше з першого. Ручний next на останньому треку без повтору — відмова «end_of_queue», відтворення триває.
//    prev на першому: з повтором — останній, без — той самий трек з початку.
//  - Перемішування: кожен трек один раз за прохід у випадковому порядку (Fisher–Yates, esp_random); обраний трек — першим.
//    Повтор усього — новий прохід (перший трек не збігається з щойно зіграним). prev — за історією (kHistoryMax), next після
//    prev повертає на покинутий трек; порожня історія — трек з початку.
//  - setShuffle() під час відтворення поточний трек не переривають: порядок перебудовується, поточний стає першим у проході.
//  - Файл, що не відкрився / збій читання: пропуск; після kMaxConsecutiveSkips поспіль — стоп «too_many_skips».
//  - Вийняття флешки / вихід із режиму: черга зупиняється (reason «media_removed» / «mode_exit»), список браузера недійсний.
//  - setShuffle / setRepeatAll живуть лише в RAM черги; [USB14] у NVS їх зберігає AppController (Settings::playerShuffle /
//    playerRepeat) і виставляє при вході в режим та за діями USB_SHUFFLE / USB_REPEAT.
//
// [USB14] ДОДАНО: пауза / продовження (requestPlayPause, Status::paused), «підготовлена» черга (Status::prepared: OK запускає
// підготовлений трек), відновлення при вході в режим (requestResume: папка -> трек -> позиція з SettingsStore, автостарт за
// player_mode_cfg::kAutoResumeOnEnter) і збереження позиції (commitResume; сама черга пише в Settings ЛИШЕ при старті треку,
// паузі та виході з режиму — у RAM-кеш SettingsStore, запис у NVS далі відкладений тією ж фоновою задачею, що й blob).
// Наявні функції й поля Status не змінено (нові поля — лише в кінець).
//
// [USB18a] ДОДАНО: Status::pos (лише в кінець). Наявні функції й поля не змінено.
//
// [USB20a-1] ДОДАНО (лише в кінець / нові функції): опція області черги scope (setScope / scope; mirror збереженого значення,
// його виставляє AppController при вході в режим і з вебу) та Status::builtScope / truncated / building. У USB20a-1 черга
// ЗАВЖДИ будується з файлів однієї папки (builtScope = 1), тож scope = «all» поки лише зберігається й показується.
//
// Потоки: як usb_browser (задача usb_nav); усі функції неблокуючі й безпечні з будь-якої задачі.
// Заголовок не підключає заголовки IDF, AppController, AudioPlayer.

#include <stddef.h>
#include <stdint.h>

#include "config/features.h"
#include "config/usb_queue_scope_config.h"  // [USB20a-1]

#if FEATURE_USB_MODE

namespace usb_queue {

enum class State : uint8_t {
    Idle,      // черга не веде відтворення (не запущена або зупинена; причина — Status::reason)
    Starting,  // файл запитано, чекаємо Playing
    Playing,   // файл грає (або дограє хвіст перед автопереходом)
};

struct Status {
    State state;
    bool shuffle;
    bool repeatAll;
    uint16_t count;      // файлів у черзі
    int32_t index;       // поточний трек (номер файлу в черзі, 0..count-1); -1 — немає
    int32_t position;    // позиція в порядку проходу (0..count-1); -1 — немає
    uint8_t skips;       // пропусків поспіль
    uint32_t started;    // скільки треків дійшло до Playing з початку сеансу черги
    uint32_t version;    // змінюється при кожній зміні стану
    const char* reason;  // причина останньої зупинки (статичний рядок) або nullptr
    // [USB14] ДОДАНО (лише в кінець):
    bool paused;         // файл на паузі (AudioPlayer::filePaused())
    bool prepared;       // черга Idle, але є підготовлений трек (OK = запустити його; після відновлення без автостарту / після кінця черги)
    // [USB18a] ДОДАНО (лише в кінець):
    int32_t pos;         // 1-based позиція поточного треку в ПОТОЧНОМУ порядку проходу (shuffle: місце s_cur у перестановці, тож
                         // «трек N з count» точний і після prev / next-назад, коли position відстає); 0 — немає
    // [USB20a-1] ДОДАНО (лише в кінець):
    uint8_t builtScope;  // область ПОБУДОВАНОЇ черги: 0 — черги нема (значення за замовчуванням, безпечне до запуску воркера), 1 — folder, 2 — all
                         // (USB20a-1: завжди 0 або 1)
    bool truncated;      // черга неповна: список папки браузера було обрізано (kMaxEntries / пул імен); для «all» — ліміт (USB20a-2)
    bool building;       // черга добудовується (лише «all», USB20a-2): next / prev / shuffle -> «queue_building»; у USB20a-1 завжди false
};

const char* stateName(State s);

// Знімок. false — мьютекс зайнятий довше usb_browser_cfg::kApiLockWaitMs.
bool status(Status* out);
// Копіює повний шлях поточного треку (від кореня флешки). false — треку нема / малий буфер / мьютекс зайнятий.
bool currentPath(char* out, size_t cap);

// Запити (неблокуючі), результат — usb_browser::status().lastReqId / lastOk / lastWhy.
// playIndex: browserIndex — номер ФАЙЛУ в списку браузера (не папки); expectVersion — версія списку (0 — не перевіряти).
bool requestPlayIndex(uint16_t browserIndex, uint32_t expectVersion = 0, uint32_t* reqId = nullptr, const char** why = nullptr);
bool requestNext(uint32_t* reqId = nullptr, const char** why = nullptr);
bool requestPrev(uint32_t* reqId = nullptr, const char** why = nullptr);
bool requestStop(uint32_t* reqId = nullptr, const char** why = nullptr);

// [USB14] ДОДАНО: OK у режимі «USB». Refused — нічого не зроблено (*why: «mode_not_ready», «busy», «starting» (трек ще
// стартує), «player_refused», «nothing_to_play»); Paused / Resumed — запит паузи / продовження прийнято (виконує задача плеєра;
// стан дивись paused()); Started — підготовлений трек запущено (виконає задача usb_nav ≤ kPollMs). Неблокуюча.
enum class PlayPause : uint8_t { Refused, Paused, Resumed, Started };
PlayPause requestPlayPause(const char** why = nullptr);

// [USB14] ДОДАНО: файл зараз на паузі. Будь-яка задача.
bool paused();

// [USB14] ДОДАНО: почати відновлення (викликає AppController один раз після першого Mounted у режимі): задача usb_nav
// дочекається кореневого списку, перейде в останню папку, знайде останній трек і підготує чергу (автостарт — за
// player_mode_cfg::kAutoResumeOnEnter). Папка / трек зникли або збережено нічого — лишається корінь без автостарту, причина
// в логу «[USBQ] resume: ...». Може створити задачу usb_nav. false — не прийнято (*why: «mode_not_ready», «task_create_failed»,
// «queue_full»); можна повторити.
bool requestResume(const char** why = nullptr);

// [USB14] ДОДАНО: скинути в SettingsStore (кеш) останній відомий трек / позицію. Викликає AppController перед standby /
// «тихим» перезапуском (далі SettingsStore::flush()). Неблокуюча (мьютекс ≤ kApiLockWaitMs). false — немає що / мьютекс
// зайнятий / задача usb_nav не працює.
bool commitResume();

// Налаштування (RAM). Застосовуються задачею usb_nav не пізніше ≈ kPollMs.
void setShuffle(bool on);
bool shuffle();
void setRepeatAll(bool on);
bool repeatAll();

// [USB20a-1] ДОДАНО: опція області черги (RAM-дзеркало; збереження в NVS — SettingsStore::setPlayerScope). Діє на НАСТУПНУ
// команду play; поточну чергу не перебудовує й відтворення не перериває. Будь-яка задача.
void setScope(usb_queue_scope_cfg::Scope s);
usb_queue_scope_cfg::Scope scope();

// --- [USB22a-1] Бібліотека: плоский список усіх файлів флешки (кеш таблиць «all» з USB20a-2) -----------------------------------
// Скан запускається ліниво: перший requestLibrary() (з GET /api/usb/library) ставить прапорець, який споживає tick() у задачі
// usb_nav. Скан не залежить від черги й від scope; таблиці лишаються кешем, поки не зміниться носій / не завершиться режим USB.
// Порядок файлів = порядок черги «all» (обхід у глибину: файли папки, потім підпапки), тож i = індекс у черзі «all».
enum class LibState : uint8_t {
    None,      // кешу немає й скан не йде (наступний requestLibrary його запустить)
    Building,  // скан іде; total — скільки файлів уже знайдено
    Ready,     // кеш готовий
    Empty,     // скан завершено, файлів немає
    Error      // скан неможливий (reason); тримається до зміни носія / виходу з режиму
};
const char* libStateName(LibState s);

struct LibPageInfo {
    LibState state;
    bool stale;        // version у запиті не збігається з поточною (кеш перебудовано)
    bool truncated;    // скан відкинув частину вмісту (ліміти / глибина / помилки / час)
    uint32_t version;  // змінюється при кожному скиданні / завершенні кешу
    uint16_t total;    // Ready/Empty — файлів у кеші; Building — прогрес
    uint16_t returned; // записів у buf (лише Ready)
    const char* reason;  // статичний рядок (лише Error)
};

// Сторінка кешу: записи [offset, offset + returned) пакуються в buf як «імʼя\0папка\0» (папка — повний шлях, «/» для кореня;
// шлях файлу = папка + «/» + імʼя, для кореня — «/» + імʼя). cap ≥ usb_library_cfg::kPageBufMin. expectVersion 0 — не перевіряти.
// Безпечно з іншої задачі (async_tcp): читає під usb_nav::lock, ≤ kApiLockWaitMs. false — мьютекс зайнятий / аргументи.
bool libraryPage(uint16_t offset, uint16_t maxItems, uint32_t expectVersion, char* buf, size_t cap, LibPageInfo* out);

// Просить запустити скан бібліотеки (якщо кешу ще немає). НЕ зупиняє відтворення. false — режим не готовий (why = "mode_not_ready").
bool requestLibrary(const char** why = nullptr);

#if USB_HOST_DIAG
// Консоль: usb.q play <індекс> | next | prev | stop | stat | shuffle on|off | repeat on|off. Блокує loopTask до завершення.
// [USB14] Стан паузи / підготовленої черги / відновлення, збережені значення й лічильники записів у NVS (друкує usb.q stat і usb.mode stat).
void printPlayerStat();
bool isConsoleCommand(const char* line);
void handleConsoleLine(const char* line);
#endif

}  // namespace usb_queue

#endif  // FEATURE_USB_MODE
