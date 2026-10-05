#pragma once

// Контролер логіки режимів і розкладки керування.
// Читає EventBus у власній задачі (ядро 0, пріоритет 3) і керує AudioProcessor,
// AudioPlayer, Settings та AppState за розкладкою з розділу 5 MASTER SPEC.
// Сам нічого не малює й не чіпає DisplayManager: екрани читають AppState.
//
// [Prompt 8] Заглушку замінено.
//   ЗМІНЕНО — begin() тепер приймає вказівник на аудіопроцесор (AppController
//             ним не володіє; main.cpp створив його раніше). nullptr дозволений:
//             тоді керування звуком недоступне, решта логіки працює.
// [Prompt 13] ДОДАНО: setTone() — вхід для веб-сервера. AppController тримає власні
//             копії bass/treble/balance і публікує їх у AppState/Settings після КОЖНОЇ
//             події, тож пряма зміна чипа повз нього була б затерта наступною подією.
// [Prompt 15] ДОДАНО: beginIrLearn()/cancelIrLearn()/confirmIrOverwrite() — вхід для
//             веб-сервера до навчання IR. IR-приймач — апаратний ресурс, а навчання
//             змінює глобальну поведінку (Mode::IrLearn), тож веб не викликає
//             IrRc5::beginLearn() та інші напряму.
// [Prompt 16] ДОДАНО: beginOta()/setOtaProgress()/otaFailed() — вхід для веб-сервера до
//             OTA-оновлення. Запис прошивки веде net/web_server (Update.*), а звук
//             зупиняє, мʼютить і відновлює AppController (веб не чіпає AudioPlayer).
// [Prompt 18] ДОДАНО: runWebCommand() + типи WebCmdType/WebCommand/WebCmdResult — ЄДИНИЙ
//             вхід для команд керування з вебу (гучність, мʼют, gain, standby, вхід, плеєр,
//             вибір станції). Події EventBus для цього не годяться: вони не ідемпотентні
//             (POWER/MUTE лише перемикають) і не повертають результату, а вебу треба
//             абсолютні значення й чесний код відповіді.

#include "core/events.h"

class AudioProcessor;

// [Prompt 13] ДОДАНО: запит на зміну тембру/балансу ззовні (веб).
// Значення мають бути вже перевірені за capabilities. Поля *Ok — результат.
struct ToneUpdate {
    bool hasBass = false;
    int8_t bass = 0;
    bool hasTreble = false;
    int8_t treble = 0;
    bool hasBalance = false;
    int8_t balance = 0;
    bool bassOk = false;
    bool trebleOk = false;
    bool balanceOk = false;
};

// [Prompt 18] ДОДАНО: команда з вебу. Семантика полів залежить від type:
//   VolumeSet     value = нова гучність (шкала UI, volumeMin..volumeMax)
//   VolumeStep    value = зміна гучності (знак = напрямок), результат обрізається до меж
//   MuteSet       flag  = true -> мʼют увімкнути
//   MuteToggle    -
//   GainSet       value = нове підсилення ПОТОЧНОГО входу (gainMin..gainMax, сирі кроки)
//   StandbySet    flag  = true -> перейти в standby, false -> вийти зі standby
//   StandbyToggle -
//   InputSet      value = індекс входу (0..inputCount-1)
//   PlayerPlay    -   (вхід Radio; уже грає -> нічого не робить)
//   PlayerPause   -   (вхід Radio; зупиняє потік і скасовує перепідключення)
//   PlayerToggle  -
//   StationPlay   value = індекс станції; flag = true -> якщо активний інший вхід, перейти на Radio
//   StationNext   - (напрямок як у кнопки RIGHT)
//   StationPrev   - (напрямок як у кнопки LEFT)
enum class WebCmdType : uint8_t {
    VolumeSet,
    VolumeStep,
    MuteSet,
    MuteToggle,
    GainSet,
    StandbySet,
    StandbyToggle,
    InputSet,
    PlayerPlay,
    PlayerPause,
    PlayerToggle,
    StationPlay,
    StationNext,
    StationPrev,
};

struct WebCommand {
    WebCmdType type;
    int32_t value;
    bool flag;
};

// [Prompt 18] ДОДАНО: результат команди -> HTTP-код у вебі.
enum class WebCmdResult : uint8_t {
    Ok,           // виконано (або вже було в такому стані)          -> 200
    Busy,         // контролер не запущено / мʼютекс зайнятий         -> 503
    NotAllowed,   // зараз недопустимо (Standby, OTA, навчання IR,
                  // не вхід Radio, немає станцій); причина — reason   -> 409
    OutOfRange,   // значення поза межами (вхід, станція, гучність)   -> 400
    Unsupported,  // чип не підтримує функцію (gain)                  -> 400
};

class AppController {
public:
    // Ініціалізація: AppState::begin() (першим ділом), стан із Settings,
    // створення задачі. Викликати ПІСЛЯ EventBus::begin(), SettingsStore::begin()
    // /load(), створення аудіопроцесора та AudioPlayer::begin().
    // Повторний виклик безпечний (повертає true).
    static bool begin(AudioProcessor* processorOrNull);

    // Обробка однієї події. Основний шлях — внутрішня задача, яка сама читає
    // чергу; публічний виклик лишено для тестування. Захищено внутрішнім
    // мʼютексом, тож безпечний і з інших задач. Не робити з обробника подій.
    static void handleEvent(const Event& event);

    // [Prompt 13] ДОДАНО: під внутрішнім мʼютексом викликає setBass/setTreble/setBalance
    // процесора, оновлює власні копії, публікує AppState і відкладено зберігає Settings.
    // false — контролер не запущено або мʼютекс зайнятий довше kLockTimeoutMs (нічого не
    // змінено). true — запит оброблено; успіх кожного поля — у *Ok (false = I2C не
    // відповів або чип не підтримує функцію; копія в такому разі не змінюється).
    static bool setTone(ToneUpdate& update);

    // [Prompt 15] ДОДАНО: починає навчання дії target. Запамʼятовує поточний Mode,
    // ставить Mode::IrLearn і викликає IrRc5::beginLearn(). Звук/потік не чіпає.
    // false — контролер не запущено, мʼютекс зайнятий довше kLockTimeoutMs, навчання
    // вже триває (Waiting/Confirm/Conflict) або IrRc5::beginLearn() відмовив (дія
    // некоректна чи мапа заповнена). Якщо на екрані ще результат попереднього навчання
    // (Success/Timeout) — його знімає одразу й починає нове.
    static bool beginIrLearn(Action target);

    // [Prompt 15] ДОДАНО: скасовує навчання (IrRc5::cancelLearn()) і одразу повертає
    // збережений Mode; статус у AppState стає Idle. Поза навчанням — лише скидає
    // «липкий» результат (Success/Timeout) у Idle. Безпечно викликати завжди.
    static void cancelIrLearn();

    // [Prompt 15] ДОДАНО: підтверджує перепризначення коду в стані Conflict
    // (IrRc5::confirmOverwrite()). false — не в Conflict, контролер зайнятий або
    // IrRc5 відмовив.
    static bool confirmIrOverwrite();

    // [Prompt 16] ДОДАНО: починає OTA. Запамʼятовує поточний Mode, мʼютить атенюатори,
    // зупиняє потік (AudioPlayer::stop()) і ставить Mode::OtaUpdate з прогресом 0.
    // false — контролер не запущено, мʼютекс зайнятий довше kLockTimeoutMs, іде навчання
    // IR (Mode::IrLearn) або OTA вже триває. Тоді веб відповідає 503, нічого не починаючи.
    static bool beginOta();

    // [Prompt 16] ДОДАНО: прогрес 0..100 (значення >100 обрізається). Дешевий виклик, можна
    // щочанк: без мʼютекса контролера, у AppState пише лише при зміні значення. Поза
    // Mode::OtaUpdate нічого не робить.
    static void setOtaProgress(uint8_t percent);

    // [Prompt 16] ДОДАНО: невдача OTA. Лог, повернення збереженого Mode, розмʼют і
    // перезапуск потоку (через звичайний ramp). Якщо мʼютекс контролера зайнятий, відновлення
    // виконає найближчий tick(). Безпечно викликати, коли OTA не триває (нічого не робить).
    static void otaFailed(const char* reason);

    // [Prompt 18] ДОДАНО: виконує команду з вебу під внутрішнім мʼютексом (як setTone()).
    // Гейти узгоджені з handleEvent(): у Mode::OtaUpdate та Mode::IrLearn — завжди
    // NotAllowed; у Standby дозволені лише StandbySet/StandbyToggle. Команди входу й
    // станції при відкритому списку станцій на пристрої спершу закривають його.
    // *reasonOut (якщо не nullptr) — статичний рядок snake_case для NotAllowed/OutOfRange/
    // Unsupported/Busy: "ota_in_progress", "ir_learn_active", "standby", "not_radio_input",
    // "wifi_setup", "no_stations", "input_unavailable", "station_out_of_range",
    // "volume_out_of_range", "gain_out_of_range", "not_supported", "busy"; інакше nullptr.
    // Стан публікується в AppState і (за потреби) зберігається в Settings до повернення.
    static WebCmdResult runWebCommand(const WebCommand& cmd, const char** reasonOut = nullptr);
};
