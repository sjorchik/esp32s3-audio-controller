#pragma once

// [USB9] Режим «USB» — бекенд (крок g1): «диригент», що безпечно проводить пристрій через вхід у режим і вихід з нього.
// Режим існує лише між requestEnter() і завершенням requestExit(); у ньому USB-host піднято (usb_msc_mount::begin()),
// радіо / прохід line_path зупинено, файли відтворює AudioPlayer::playFile() (кроки USB6 / USB7).
//
// Потоки. Усе довге (очікування тиші, begin() / монтування, stop файлу, end() 0,6 – 1,25 с, відновлення входу) виконує
// ОДНОРАЗОВА фонова задача usb_mode (ядро 0, пріоритет 1, див. config/usb_mode_config.h), яка сама видаляється. Публічні
// функції не блокують (крім невеликої діагностики): їх можна викликати з будь-якої задачі, зокрема з AppController (він
// викликає requestExit() при standby). Стан читається потокобезпечно з будь-якої задачі.
//
// Стани: Off -> Entering -> Ready -> Exiting -> Off; Error — стек знято невдало / вхід не відкотився (потрібен повтор exit
// за станом end_not_quiet або перезавантаження). Playing — ПОХІДНИЙ стан (Ready + AudioPlayer::fileActive()). «Флешки
// нема» — не стан, а Media::None у Ready; вийняття флешки лишає в режимі (автовиходу немає), файл зупиняє сам AudioPlayer
// (PlayerState::Error + lastFileError()), повторна вставка монтується сама (host лишається піднятим).
//
// Заголовок не підключає заголовки IDF, AppController, AudioPlayer.

#include <stdint.h>

#include "config/features.h"

#if FEATURE_USB_MODE

namespace usb_mode {

enum class State : uint8_t {
    Off,       // режиму немає (USB-host не піднято, якщо немає Error попереднього сеансу — див. info().reason)
    Entering,  // іде вхід: тиша радіо / проходу, begin(), очікування носія
    Ready,     // режим активний; media — носій є чи ні
    Playing,   // ПОХІДНИЙ: Ready + файл відтворюється (AudioPlayer::fileActive())
    Exiting,   // іде вихід: stop файлу, end(), відновлення попереднього входу
    Error,     // збій (info().reason / detail); див. requestExit()
};

enum class ExitCause : uint8_t {
    User,     // команда користувача (консоль; у кроці g2 — UI)
    Standby,  // AppController увійшов у standby: файл уже зупинено, повернення на вхід не потрібне
};

// Стан носія (похідний від usb_msc_mount::state()).
enum class Media : uint8_t {
    None,      // носія нема (або стек не піднято)
    Mounting,  // носій вставлено, іде монтування
    Mounted,   // том змонтовано
    Fault,     // носій є, але змонтувати не вдалося (usb_msc_mount::lastError())
};

struct RamSample {
    uint32_t freeBytes;     // heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
    uint32_t largestBlock;  // heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
    uint32_t minFreeBytes;  // heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
};

struct Info {
    State state;
    const char* reason;   // причина Error або останнього невдалого входу / виходу (статичний рядок); nullptr — не було
    const char* detail;   // уточнення (usb_msc_mount::lastError().where або причина відмови AppController); nullptr
    Media media;
    int16_t prevInput;    // вхід до входу в режим; -1 — режиму не було
    uint16_t prevStation;
    bool prevRadioPaused;
    bool exitQueued;      // exit запитано під час Entering: виконається одразу після входу
    bool needsReboot;     // usb_msc_mount::needsReboot()
    // Тривалості етапів останнього входу / виходу, мс (0 — не виконувався).
    uint32_t quietMs;     // від запиту до тиші радіо / проходу
    uint32_t beginMs;     // usb_msc_mount::begin()
    uint32_t mountMs;     // очікування носія після begin() (до змонтування / «флешки нема» / збою)
    uint32_t enterMs;     // увесь вхід
    uint32_t fileStopMs;  // зупинка файлу (AudioPlayer Idle)
    uint32_t endMs;       // usb_msc_mount::end()
    uint32_t restoreMs;   // відновлення попереднього входу (виклик AppController)
    uint32_t exitMs;      // увесь вихід
    // Внутрішня RAM: ДО входу (радіо / прохід ще працюють), після «тиші» (до begin()), коли Ready, після end().
    RamSample ramBeforeEnter;
    RamSample ramQuiet;
    RamSample ramReady;
    RamSample ramAfterEnd;
    uint32_t taskStackMinFree;  // мін. вільний стек задачі usb_mode в останній операції, байт (0 — не вимірювався)
};

// Запит на вхід. Неблокуючий. true — прийнято (стан Entering; результат — state() / info()). false — відмова: *whyOut
// (якщо не nullptr) — статичний рядок («not_off», «busy», «task_create_failed»). Повторний запит у Ready / Entering —
// нешкідливо (false з «not_off», нічого не змінюється).
bool requestEnter(const char** whyOut = nullptr);

// Запит на вихід. Неблокуючий. true — прийнято, або вже Off / Exiting (нешкідливо). Під час Entering запит ставиться в чергу
// (exitQueued) і виконається одразу після входу. У Error: повтор end() дозволено лише за lastError().where = «end_not_quiet»
// / «end_not_quiet_post» (стек справний); інакше false (потрібне перезавантаження), а для cause = Standby лише знімається
// прапорець у AppController (щоб пристрій міг вийти зі standby).
bool requestExit(ExitCause cause = ExitCause::User);

// Поточний стан (Playing — похідний). Потокобезпечно, не блокує.
State state();

// true — режим займає шлях звуку (Entering / Ready / Playing / Exiting / Error).
bool isActive();

// [USB11] ДОДАНО: лише стан носія (без копіювання Info; дешево — для періодичної публікації стану в UI з AppController).
// Media::None, якщо режиму нема.
Media media();

// Знімок усього стану й вимірів. Потокобезпечно (коротка критична секція), не блокує.
Info info();

const char* stateName(State s);
const char* mediaName(Media m);

#if USB_HOST_DIAG
// Консоль (викликає usb_play_diag за рядками, що починаються з «usb.mode»): enter | exit | stat | cycle <N>.
// Викликається з loopTask; `cycle` блокує його на час N циклів (як usb.life).
bool isModeCommand(const char* line);
void handleConsoleLine(const char* line);
#endif

}  // namespace usb_mode

#endif  // FEATURE_USB_MODE
