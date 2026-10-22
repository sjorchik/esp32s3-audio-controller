#pragma once

// Контролер логіки режимів і розкладки керування.
// Читає EventBus у власній задачі (ядро 0, пріоритет 3) і керує входами (реле-селектор), AudioPlayer,
// цифровим трактом, Settings та AppState за розкладкою з розділу 5 MASTER SPEC.
// Сам нічого не малює й не чіпає DisplayManager: екрани читають AppState.
//
// [Prompt 48a] ЗМІНЕНО: аудіочіпи (TDA7318 / PT2313L) вирізано. begin() БЕЗ параметрів (було begin(AudioProcessor*)).
//             Входів 3 (0 радіо, 1 TV Box, 2 Computer), 1 / 2 комутує реле (audio/input_selector). setTone() і
//             WebCmdType::GainSet більше нічого не приймають: Unsupported (тип і метод лишено заради ABI).
// [Prompt 50b] ВИДАЛЕНО: ToneUpdate, AppController::setTone(), WebCmdType::GainSet, WebCmdResult::Unsupported.
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
// [Prompt 21b] ЗМІНЕНО: ToneUpdate розширено полями hasLoudness/loudness/loudnessOk (лише в кінець,
//             з дефолтами — наявний код лишається чинним). Тонкомпенсацію тепер веде AppController,
//             а не веб напряму. Усі параметри звуку (гучність, бас, дискант, баланс, gain,
//             loudness) зберігаються ПО ВХОДАХ: будь-яка зміна пише в профіль поточного входу, а
//             при перемиканні входу (з будь-якого джерела), виході зі standby й на старті
//             профіль входу застосовується до чипа ще під мʼютом. Публічні методи не змінено.

// [Prompt 28] ДОДАНО: startOffline() — запуск офлайн-режиму (Wi-Fi вимкнено, усі входи, крім
//             радіо). Також (без зміни публічного інтерфейсу) довге утримання POWER
//             (Event::veryLongPress) запускає «тихий» перезапуск у будь-якому режимі.
// [Prompt 39] ДОДАНО: WebCmdType::InputsEnabledSet (маска дозволених входів 1..3, лише з вебу) і
//             reason "input_disabled" для InputSet. Холодний старт (скидання за живленням) тепер
//             одразу в Standby: логіка — у begin() (публічний інтерфейс не змінено).

// [Prompt 47c] ДОДАНО: applyLineEq(). Зовнішні входи 1..4 звучать через audio/line_path: AppController сам керує
//             проходом (старт, гучність, мʼют користувача, EQ входу, перемикання, звільнення ресурсів).
//             Решту публічного інтерфейсу не змінено.
// [Prompt 48b] ДОДАНО: WebCmdType::BalanceSet (баланс активного входу, дБ), reason "balance_out_of_range". Баланс — ЦИФРОВИЙ,
//             індивідуальний для кожного входу (Settings::profiles[i].balanceDb), застосовується в DSP радіо
//             (radio_volume) або проходу (LinePath). Керування: клік енкодера перемикає ціль Volume <-> Balance, IR
//             BALANCE_UP / BALANCE_DOWN, веб. Решту публічного інтерфейсу не змінено.
// [Prompt 48c] ДОДАНО: prepareForRestart() — «тихий» перезапуск для веб reboot / factory-reset / wifi-reset, OTA й порталу Wi-Fi.
// [V1] ДОДАНО: WebCmdType::VuSensSet (чутливість VU АКТИВНОГО входу, дБ), reason "vu_sens_out_of_range". Чутливість VU —
//             зсув рівня показу в дБ (vu_sens_cfg: ±20, крок 1, 0 за замовчуванням), на звук не впливає; зберігається в
//             профілі входу (Settings::profiles[i].vuSensDb, NVS v8) і застосовується до VU при кожному застосуванні
//             профілю (старт, зміна входу, вихід зі standby). Керування — лише веб; кнопок / енкодера / IR немає.
//             Решту публічного інтерфейсу не змінено.
// [Prompt 46a] Чип — лише селектор входів; профіль входу = {гучність, пресет EQ}. ПУБЛІЧНІ СИГНАТУРИ НЕ ЗМІНЕНО,
//             змінилась лише семантика: setTone() приймає й ІГНОРУЄ тембр/баланс/loudness (*Ok = true для
//             запитаних полів, які мав чип; у чіп, профіль і AppState нічого не йде); WebCmdType::GainSet
//             приймається й ігнорується (Ok); дії BASS_* .. GAIN_* ігноруються; цикл цілей енкодера прибрано
//             (ціль завжди гучність). [P48a] Нейтральні значення чіпа (kNeutral*) видалено разом із чіпом.

#include "config/eq_config.h"  // [Prompt 47c] eq_cfg::kBandCount для applyLineEq()
#include "core/events.h"

// [Prompt 18] ДОДАНО: команда з вебу. Семантика полів залежить від type:
//   VolumeSet     value = нова гучність (шкала UI, volumeMin..volumeMax)
//   VolumeStep    value = зміна гучності (знак = напрямок), результат обрізається до меж
//   MuteSet       flag  = true -> мʼют увімкнути
//   MuteToggle    -
//   StandbySet    flag  = true -> перейти в standby, false -> вийти зі standby
//   StandbyToggle -
//   InputSet      value = індекс входу (0..2: радіо, TV Box, Computer)
//   PlayerPlay    -   (вхід Radio; уже грає -> нічого не робить)
//   PlayerPause   -   (вхід Radio; зупиняє потік і скасовує перепідключення)
//   PlayerToggle  -
//   StationPlay   value = індекс станції; flag = true -> якщо активний інший вхід, перейти на Radio
//   StationNext   - (напрямок як у кнопки RIGHT)
//   StationPrev   - (напрямок як у кнопки LEFT)
//   BalanceSet    value = баланс АКТИВНОГО входу, дБ (balance_cfg::kMinDb..kMaxDb; додатне = вправо) [Prompt 48b].
//                 Поза діапазоном -> OutOfRange ("balance_out_of_range"); у Standby -> NotAllowed ("standby").
//   VuSensSet     value = чутливість VU АКТИВНОГО входу, дБ (vu_sens_cfg::kMinDb..kMaxDb) [V1]. Поза діапазоном ->
//                 OutOfRange ("vu_sens_out_of_range"); у Standby -> NotAllowed ("standby"); OTA / навчання IR — як інші
//                 команди. НЕактивний вхід обробник пише лише в Settings (як баланс), без runWebCommand.
//   InputsEnabledSet  value = нова МАСКА дозволених входів (біт i = вхід i; значення нормалізується:
//                     біт 0 завжди 1, зайві біти скидаються) [Prompt 39]. Єдина команда, дозволена
//                     в Standby (крім живлення). Якщо активний вхід став вимкненим — перехід на
//                     перший доступний (радіо) штатним changeInput(); зберігається в Settings.
enum class WebCmdType : uint8_t {
    VolumeSet,
    VolumeStep,
    MuteSet,
    MuteToggle,
    StandbySet,
    StandbyToggle,
    InputSet,
    PlayerPlay,
    PlayerPause,
    PlayerToggle,
    StationPlay,
    StationNext,
    StationPrev,
    InputsEnabledSet,  // [Prompt 39] ДОДАНО (лише в кінець enum)
    BalanceSet,        // [Prompt 48b] ДОДАНО (лише в кінець enum)
    VuSensSet,         // [V1] ДОДАНО (лише в кінець enum)
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
};

class AppController {
public:
    // Ініціалізація: AppState::begin() (першим ділом), стан із Settings,
    // створення задачі. Викликати ПІСЛЯ EventBus::begin(), SettingsStore::begin()
    // /load(), input_selector::begin(), AudioPlayer::begin() і LinePath::begin().
    // Повторний виклик безпечний (повертає true).
    static bool begin();

    // Обробка однієї події. Основний шлях — внутрішня задача, яка сама читає
    // чергу; публічний виклик лишено для тестування. Захищено внутрішнім
    // мʼютексом, тож безпечний і з інших задач. Не робити з обробника подій.
    static void handleEvent(const Event& event);

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

    // [Prompt 18] ДОДАНО: виконує команду з вебу під внутрішнім мʼютексом.
    // Гейти узгоджені з handleEvent(): у Mode::OtaUpdate та Mode::IrLearn — завжди
    // NotAllowed; у Standby дозволені лише StandbySet/StandbyToggle. Команди входу й
    // станції при відкритому списку станцій на пристрої спершу закривають його.
    // *reasonOut (якщо не nullptr) — статичний рядок snake_case для NotAllowed/OutOfRange/
    // Busy: "ota_in_progress", "ir_learn_active", "standby", "not_radio_input",
    // "wifi_setup", "no_stations", "input_unavailable", "station_out_of_range",
    // "volume_out_of_range", "balance_out_of_range" [Prompt 48b], "vu_sens_out_of_range" [V1], "busy"; [Prompt 39] + "input_disabled"
    // (InputSet на вхід, вимкнений маскою входів -> NotAllowed); інакше nullptr.
    // Стан публікується в AppState і (за потреби) зберігається в Settings до повернення.
    static WebCmdResult runWebCommand(const WebCommand& cmd, const char** reasonOut = nullptr);

    // [Prompt 28] ДОДАНО: запускає офлайн-режим. Працює ЛИШЕ в Mode::WifiSetup (екран
    // налаштування Wi-Fi): зупиняє AP/портал/Wi-Fi (WifiManager::stop()) і переводить
    // пристрій у робочий стан на першому доступному вході, що не є радіо (мʼют -> профіль
    // входу -> розмʼют із ramp). Радіо після цього недоступне до перезапуску; у NVS режим НЕ
    // записується. false — контролер не запущено, мʼютекс зайнятий довше kLockTimeoutMs, режим
    // не WifiSetup, уже офлайн або йде перезапуск. Основний шлях виклику — подія OK / клік
    // енкодера у WifiSetup всередині контролера; публічний метод — для тестів і майбутніх входів.
    static bool startOffline();

    // [Prompt 48c] ДОДАНО: «тихий» перезапуск для викликачів поза контролером (веб reboot / factory-reset /
    // wifi-reset, успішний OTA, портал Wi-Fi). Та сама послідовність, що в «тихому» перезапуску POWER (P28):
    // цифровий мʼют радіо / проходу -> стоп потоку -> пін підсилювача LOW (через amp_cfg::kAmpOffDelayMs) ->
    // пауза app_controller_cfg::kRestartMuteMs; застосовані налаштування скидаються в NVS (flush). На відміну від
    // POWER, ESP.restart() НЕ викликає — це робить викликач після повернення (щоб встигнути, наприклад,
    // SettingsStore::resetToDefaults()). БЛОКУЄ викликаючу задачу до kPrepareRestartWaitMs (≈ 0,5 с у нормі):
    // викликати ЛИШЕ з окремої задачі (api_restart / ota_restart / wifi), НЕ з HTTP-обробника AsyncTCP і не
    // з задачі контролера. Після виклику контролер ігнорує події й веб-команди (503 busy), екран показує
    // «Restarting...», і лишається замʼючений до ESP.restart().
    // true — звук замʼючено й пауза витримана (або перезапуск уже веде сам контролер з POWER-утримання).
    // false — контролер не запущено, мʼютекс зайнятий довше kLockTimeoutMs або вичерпано kPrepareRestartWaitMs;
    // тоді викликач усе одно перезапускається (як раніше), просто без гарантії тиші.
    static bool prepareForRestart();

    // [Prompt 47c] ДОДАНО: живе застосування пресету EQ зовнішнього входу. Веб-обробник /api/eq спершу пише
    // пресет у Settings, потім для input != 0 викликає це: якщо input збігається з ПОТОЧНИМ зовнішнім
    // входом — LinePath::setEqAllDb(gains) одразу (без перезапуску проходу); інакше нічого (пресет
    // застосується при перемиканні на цей вхід). input == 0 (радіо) обробляє сам обробник через eq::setAllDb.
    // false — контролер не запущено або мʼютекс зайнятий довше kLockTimeoutMs (пресет у Settings збережено,
    // звук оновиться при наступному перемиканні входу). При LINE_PATH_ENABLE == 0 — нічого не робить (true).
    static bool applyLineEq(uint8_t input, const int8_t gains[eq_cfg::kBandCount]);
};
