// AppController: центральний координатор (Prompt 8).
//
// Усі переходи з очікуванням (мʼют -> зміна -> розмʼют) реалізовано як
// невеликий автомат Phase, який рухає tick() за збереженим часом, а не
// блокуючими затримками. Один внутрішній рекурсивний мʼютекс захищає і
// handleEvent(), і tick().
//
// [Prompt 11] Список станцій береться зі StationStore (LittleFS), а не з
// тестових констант плеєра.
//
// [Prompt 12] Стан Wi-Fi береться з WifiManager (прямі виклики WiFi.* прибрано).
// [Prompt 36] syncPlayer() додатково публікує AppState.wifiRssi (WifiManager::rssi(), квантування
// wifi_status_cfg) — для іконки рівня сигналу на екрані Radio.
// Коли WifiManager піднімає AP, а вхід — Radio, контролер переходить у
// Mode::WifiSetup (екран з назвою AP та адресою). Зовнішні входи від Wi-Fi не
// залежать і лишаються робочими; зі WifiSetup можна перемикати вхід і міняти
// гучність.
//
// [Prompt 13] ДОДАНО: AppController::setTone() для веб-сервера (див. app_controller.h).
//
// [Prompt 14] Список станцій може змінитись з вебу (видалення/перестановка/імпорт),
// тож s_station і s_menuSel можуть вказувати за межу списку. Додано
// clampStationIndex(): обрізає s_station до [0, count-1] (при count == 0 нічого
// не робить — це обробляють окремі перевірки) і викликається перед кожним
// використанням індексу та періодично з tickLocked(). Публічний API не змінено.
//
// [Prompt 15] ДОДАНО: навчання IR-пульта (beginIrLearn/cancelIrLearn/confirmIrOverwrite).
// Mode::IrLearn тепер реально вмикається: попередній Mode запамʼятовується й
// відновлюється після навчання. Звук і потік під час навчання НЕ чіпаємо (мʼют, ramp,
// плеєр — як були); для мʼюту «логічний Standby» = Standby АБО навчання, розпочате
// зі Standby (logicallyStandby()). Статус IrRc5 синхронізується в AppState щотакту
// tickLocked() (а не раз на kSyncPeriodMs), щоб екран і веб бачили кроки навчання без
// затримки; поки триває навчання, задача опитує чергу з активним періодом.
// Success/Timeout показуються ir_learn_ui_cfg::kResultHoldMs і лишаються в AppState як
// «останній результат» до нового навчання чи cancelIrLearn().
//
// [Prompt 16] ДОДАНО: OTA-оновлення (beginOta/setOtaProgress/otaFailed). Mode::OtaUpdate:
// звук зупинено (stopStream), атенюатори замʼючені (desiredMute()), усі події ігноруються
// (навіть POWER), setTone()/beginIrLearn() відмовляють. Попередній Mode запамʼятовується й
// відновлюється лише на невдачі; на успіху пристрій перезапускає web_server. Якщо
// otaFailed() не отримав мʼютекс, відновлення робить найближчий tickLocked().
//
// [Prompt 18] ДОДАНО: runWebCommand() — команди керування з вебу (абсолютна гучність, gain,
// мʼют/standby як set, вхід, play/pause, вибір станції за індексом). Переиспользує ті самі
// внутрішні дії, що й обробники подій (setVolumeTarget, changeInput, changeStation,
// stepStation, togglePlayPause, enterStandby/leaveStandby, adjustGain), тож поведінка
// (ramp, мʼют на переходах, збереження в Settings) ідентична кнопкам/пульту.
//
// [Prompt 20b] AppState.stationName = НАЗВА ЗІ СПИСКУ станцій (StationStore) для поточного
// s_station; ICY-назва потоку (AudioPlayer::currentMetadata) — лише запасний варіант, якщо
// назви у списку немає (порожній список / індекс поза межами / порожня назва). Усе вирішення
// — у resolveStationName(); споживачі (екран, /api/status) читають готове поле. Оновлення:
// кожен період syncPlayer() (ловить будь-яку зміну списку з вебу: перейменування, переміщення,
// видалення, імпорт) + негайно в publishStationName() при старті/виборі станції та зміні входу.
// Публічний API, AppStateData і StationStore не змінено.

// [Prompt 21b] Профілі звуку по входах. Гучність, бас, дискант, баланс, gain і loudness
// зберігаються й застосовуються ОКРЕМО для кожного входу. s_profiles[] — повна таблиця
// профілів (копія Settings::profiles); s_volume/s_bass/s_treble/s_balance/s_gain/s_loudness —
// робочі значення ПОТОЧНОГО входу (те, що в чипі й у AppState). Єдине місце запису в профіль —
// persist() (через captureActiveProfile()): усі джерела змін (енкодер, пульт, веб, setTone,
// runWebCommand) уже закінчують шлях викликом persist(). Перемикання входу з будь-якого
// джерела йде через changeInput(): профіль старого входу фіксується, потім під мʼютом
// (startTransition) вхід -> профіль нового входу (тембр, баланс, gain, loudness) -> розмʼют
// kUnmuteDelayMs -> ramp гучності до значення нового профілю. Те саме — при виході зі standby
// і на старті (powerOnTransition). Мʼют користувача глобальний і в профіль не входить.
// gain тепер веде сам контролер (s_gain), а не cachedState() драйвера.
//
// [Prompt 23b] AppState.inputName = користувацька назва ПОТОЧНОГО входу (Settings::inputNames),
// запасно defaults::kInputNames. ЄДИНЕ місце вирішення — resolveInputName(). Публікується в
// publishState() (старт, зміна входу з будь-якого джерела, вихід зі standby — усі ці шляхи
// закінчуються publishState()) і щоперіодно в syncPlayer() (kSyncPeriodMs): так назва, змінена
// на веб-сторінці (/api/settings пише лише в SettingsStore і контролер не повідомляє), зʼявляється
// на дисплеї без перемикання входу. Читання Settings — лише тут, у задачі контролера.
//
// [Prompt 23c] Пін standby підсилювача (AmpStandby, GPIO46: 1 = працює, 0 = standby). Пін у LOW
// з початку setup() (main.cpp). Вмикання (powerOnTransition: старт і вихід зі standby): якщо
// підсилювач вимкнений — пін HIGH лише ПІСЛЯ підтвердженого мʼюту атенюаторів, далі Settle не
// завершується, доки не мине kAmpWakeMs (мʼют лишається), і лише тоді розмʼют із ramp. Якщо
// підсилювач уже працює (зміна входу/станції, швидке повторне вмикання) — прогріву немає.
// Standby (enterStandby): штатний мʼют, через kAmpOffDelayMs пін LOW (за непідтвердженого мʼюту —
// не раніше kAmpOffMuteWaitMs). Усе без блокувань: стан рухає ampService() із tickLocked().
// Мʼют користувача, гучність, вхід, станція, OTA і IR пін не чіпають.

// [Prompt 28] Офлайн-режим і «тихий» перезапуск.
// Офлайн: у Mode::WifiSetup OK (кнопка / IR) або клік енкодера викликає startOfflineLocked():
// s_offline = true, WifiManager::stop() (AP, портал, WiFi.mode(WIFI_OFF)), далі changeInput() на
// перший вхід, що не є радіо (мʼют -> профіль входу -> розмʼют/ramp; підсилювач уже працює).
// «Радіо недоступне» вирішується в ОДНОМУ місці — inputAvailable(): його використовують
// stepInput(), selectInput(), веб InputSet/StationPlay, leaveStandby() і сторожа в changeInput();
// запуск потоку в офлайні блокує сам startStream(). Режим не зберігається в NVS.
// Перезапуск: подія POWER з veryLongPress (драйвер кнопок, input_cfg::kPowerRestartHoldMs) у БУДЬ-ЯКОМУ
// режимі: мʼют атенюаторів (desiredMute() через restarting()) + стоп потоку -> ampRequestOff()
// (пін LOW через kAmpOffDelayMs, P23c) -> пауза kRestartMuteMs -> ESP.restart(). Усе кроками
// restartService() із tickLocked(), без блокувань; kRestartMaxWaitMs — страховка.

// [Prompt 32] Спливне вікно параметра: notifyPopup()/popupForEvent() у handleLocked() (лише локальні
// джерела: енкодер, кнопки, IR); AppState.popupTarget/popupSeq. Веб (runWebCommand) вікно не викликає.
// [Prompt 31] Автоповернення цілі регулювання енкодера на гучність. Будь-яка подія енкодера
// (ENC_CW / ENC_CCW / ENC_PRESS, зокрема довге утримання = мʼют) оновлює s_lastEncMs; коли
// s_target != Volume і від останньої події енкодера минуло kAdjustTimeoutMs, tickLocked()
// повертає ціль на гучність і публікує стан (екран малює ціль за AppState.adjustTarget).
// Також ціль скидається на гучність при вході в Standby й при вмиканні (powerOnTransition).
// Публічний інтерфейс не змінено; у NVS ціль не зберігається.

// [Prompt 39] Холодний старт у standby і відключення зовнішніх входів.
// 1) begin(): якщо esp_reset_reason() ∈ app_controller_cfg::kColdStartResetReasons (подача живлення,
//    brownout) — гілка «boot in standby» (та сама, що для kBootInStandby): мʼют, пін підсилювача LOW
//    (з setup()), потік не запускається, жодного powerOnTransition(). Вихід — POWER штатно
//    (leaveStandby: lastInput/lastStation/профіль входу). ESP.restart() (веб reboot / OTA / «тихий»
//    перезапуск), паніка, watchdog тощо — поведінка без змін. Стан живлення в NVS НЕ зберігається.
// 2) Маска дозволених входів (Settings::inputEnabledMask, біт 0 = радіо завжди 1) живе в s_inputMask
//    (дзеркало Settings; пише лише цей файл через WebCmdType::InputsEnabledSet). ЄДИНЕ місце рішення
//    «чи доступний вхід» лишається inputAvailable(): тепер = апаратно є (inputCount) І дозволено маскою
//    І (в офлайні) не радіо. Тому циклічне перемикання (UP/DOWN), пряме вибирання (INPUT_*), веб
//    InputSet, відновлення lastInput (begin, leaveStandby) автоматично пропускають вимкнені входи.
//    Якщо вимикається активний вхід — перехід на перший доступний (радіо) через changeInput()
//    (у Standby — лише s_input і профіль, без чіпа). Веб InputSet на вимкнений вхід -> NotAllowed
//    "input_disabled" (409). Офлайн-режим не запускається, коли жодного зовнішнього входу не дозволено
//    (інакше пристрій лишився б без звуку на екрані WifiSetup до перезапуску).

// [Prompt 46a] Чип (TDA7318 / PT2313L) лишається ЛИШЕ селектором входів. Профіль входу = {гучність,
// пресет EQ}; контролер тримає лише гучності (s_volumes[]), пресет EQ живе в Settings і не дублюється:
// його пише веб-обробник /api/eq (SettingsStore::modify), а persist() записує в профілі ТІЛЬКИ гучність
// (інакше застарілу копію EQ було б записано поверх свіжої). Бас / дискант / баланс / gain / loudness
// прибрано з профілю, подій, команд і цілей енкодера; у чіп при застосуванні профілю надсилаються фіксовані
// нейтральні значення (P48a: app_controller_cfg::kLegacy*), а поля AppStateData bass/treble/balance/gain/loudness
// публікуються цими ж нейтральними константами (застарілі заглушки до P49/P50). Ціль регулювання енкодера
// завжди Volume (цикл цілей і автоповернення P31 прибрано); короткий клік енкодера лише показує вікно
// гучності. Дії BASS_* .. GAIN_* (enum незмінний) ігноруються з одним коротким логом. Пресет EQ екземпляра
// радіо = profiles[0].eqGainsDb: застосовується в SettingsStore::load()/resetToDefaults(), у /api/eq при
// поточному вході 0 і applyRadioEq() при поверненні на вхід 0 / вмиканні на ньому.

// [Prompt 47a] ЦИФРОВА ГУЧНІСТЬ РАДІО. Підсилювач сидить на виході PCM5102, тож гучність радіо — це
// audio/radio_volume (DspVolume у хуку PCM, ядро 1): зміна гучності = setAttenuationDb(radioVolumeDb(vol)),
// мʼют = setMute() (плавно: 20 мс вниз, 100 мс вгору — рампу робить DSP, програмної рампи в контролері
// більше немає, Phase::Ramp прибрано). Чип гучність НЕ отримує (P48a: чипа немає). Цифровий мʼют дзеркалить desiredMute() (syncRadioMute()), і ПЕРЕД
// зупинкою плеєра / зміною станції / вимкненням підсилювача контролер чекає згасання (до kRadioMuteWaitMs;
// лише коли радіо грає): startTransition(), enterStandby(), beginRestart(), beginOta(), pauseRadio().
// Пауза радіо тримає DSP у мʼюті (s_stopHold) до наступного startStream(), щоб старий потік, що
// дограє після асинхронного stop(), не "виринув" з нового наростання. Екземпляр DSP стартує замʼюченим.

// [Prompt 47c] ЗОВНІШНІ ВХОДИ 1..4 ЧЕРЕЗ audio/line_path. Вибір зовнішнього входу запускає прохід PCM1808 -> DSP -> PCM5102,
// AppController сам його веде автоматом LineState (без блокувань, рух — lineService() у tickLocked()):
//   Off        — проходу немає (радіо / standby / перезапуск / OTA);
//   Starting   — WaitIdle (чекаємо AudioPlayer Idle; stop() асинхронний) -> requestStart() -> WaitRunning
//                (state()==Running і вихід чіпа стабілізовано) | WaitRetry (пауза перед єдиним автоповтором);
//   Active     — звучить; цифровий мʼют користувача = LinePath::setUserMute(desiredMute());
//   Switching  — вхід<->вхід БЕЗ перезапуску проходу: мʼют -> kExtMuteSettleMs -> setInput чіпа + гучність + EQ входу
//                -> kExtInputSettleMs -> Active (мʼют знімає syncLineMute());
//   Releasing  — мʼют -> kExtMuteSettleMs -> returnToRadio() -> чекаємо resourcesUp()==false (kLineReleaseTimeoutMs),
//                далі за ситуацією: радіо (startTransition + стрім, щоб RAM під TLS уже була вільна) | новий старт проходу |
//                Off (standby / OTA / перезапуск);
//   Failed     — старт не вдався двічі: тиша до наступного явного вибору входу (автоповернення на радіо НЕМАЄ).
// Мʼют проходу = desiredMute() || state != Active (lineMuteWanted()). Phase::Settle не завершується, поки прохід
// Starting / Switching / Releasing (lineBusy()). Вхід<->вхід не запускає Phase::Settle (250 мс потрібні лише для аналогового
// чипа / підсилювача). Чип-селектор міняється лише тоді, коли прохід цифрово замʼючений (applyChipForInput()).
// XSMT AppController у режимі проходу не чіпає. Усі виклики LinePath — неблокуючі (requestStart, returnToRadio,
// setUserMute, setVolumeDb, setEqAllDb). При LINE_PATH_ENABLE == 0 жодного виклику LinePath немає: зовнішні входи мовчать,
// поведінка радіо як у P47a.

// [Prompt 48a] АУДІОЧІПИ (TDA7318 / PT2313L) І I2C ВИРІЗАНО. AppController працює без AudioProcessor:
// begin() без параметрів; логічних входів 3 (0 радіо, 1 TV Box, 2 Computer), «вхід чіпа k - 1» скасовано.
// Входи 1 / 2 комутує реле (audio/input_selector::select()) у вже наявній послідовності P47c ПІД цифровим
// мʼютом проходу: мʼют -> kExtMuteSettleMs -> select() -> input_selector_cfg::kRelaySettleMs -> розмʼют.
// Радіо реле НЕ чіпає. Чип-мʼют (setMute) прибрано: s_hwMute = «цифровий мʼют виставлено» (потрібен
// піну підсилювача P23c). Тембр / gain / loudness видалено: WebCmdType::GainSet -> Unsupported, setTone()
// нічого не приймає. Профіль входу = {volume, eqGainsDb[5], balanceDb}: контролер пише лише гучність;
// balanceDb поки не чіпає (P48b). inputAvailable() = маска входів (+ офлайн: без радіо).
// IR прямого вибору: INPUT_RADIO / INPUT_TV / INPUT_PC -> 0 / 1 / 2; INPUT_AUX — без дії.

#include "core/app_controller.h"

#include <Arduino.h>
#include <esp_system.h>  // [Prompt 39] esp_reset_reason()
#include <string.h>

#include "audio/amp_standby.h"  // [Prompt 23c]
#include "audio/audio_player.h"
#include "audio/eq.h"  // [Prompt 46a] applyRadioEq()
#include "audio/input_selector.h"  // [Prompt 48a] реле-селектор входів 1 / 2
#include "audio/line_path.h"     // [Prompt 47c]
#include "audio/radio_volume.h"  // [Prompt 47a]
#include "config/amp_config.h"  // [Prompt 23c]
#include "config/app_controller_config.h"
#include "config/defaults.h"
#include "config/features.h"  // [Prompt 23c]
#include "config/input_selector_config.h"  // [Prompt 48a] kRelaySettleMs
#include "config/ir_learn_ui_config.h"  // [Prompt 15] ДОДАНО
#include "config/line_path_config.h"  // [Prompt 47c] LINE_PATH_ENABLE
#include "config/audio_config.h"     // [P47a] radioVolumeDb(); [P48a] kVolumeUiMin / kVolumeUiMax
#include "config/settings_config.h"  // [Prompt 39] normalizeInputMask, kInputMaskAll
#include "config/wifi_status_config.h"  // [Prompt 36]
#include "core/app_state.h"
#include "core/settings.h"
#include "input/ir_rc5.h"            // [Prompt 15] ДОДАНО
#include "net/wifi_manager.h"       // [Prompt 12] ДОДАНО
// [Prompt 25] Рівень виходу декодера по станціях (Station::levelDb). startStream() — ЄДИНЕ місце
// запуску потоку (старт, standby, вибір зі списку, LEFT/RIGHT, веб StationPlay, відкладений
// старт без Wi-Fi, зміна входу): воно виставляє рівень станції через
// AudioPlayer::setOutputTrimDb() ДО playUrl(), тож звук починається вже з потрібним рівнем.
// Живе редагування: syncStationLevel() у періодичному блоці tickLocked() порівнює рівень
// станції за s_station з застосованим і, якщо URL станції збігається з URL потоку, що грає,
// змінює рівень наживо (плавно, без перепідключення). Індекси позиційні (як у P20b): після
// move/remove станція за s_station може бути іншою, ніж та, що грає, — тоді URL не збігається
// й рівень НЕ чіпаємо (потік і його рівень узгоджені до наступного старту станції).
// Публічний API AppController, AppStateData, Settings і NVS не змінено.
#include "stations/station_store.h"  // [Prompt 11] ДОДАНО

namespace cfg = app_controller_cfg;

// [Prompt 28] Страховка перезапуску має перекривати найдовше очікування піна підсилювача (P23c).
static_assert(app_controller_cfg::kRestartMaxWaitMs >
                  amp_cfg::kAmpOffMuteWaitMs + app_controller_cfg::kRestartMuteMs,
              "kRestartMaxWaitMs must exceed amp off wait + restart pause");

// [Prompt 47c] Пауза мʼюту проходу вкладається в затримку вимкнення підсилювача при standby.
static_assert(app_controller_cfg::kExtMuteSettleMs < amp_cfg::kAmpOffDelayMs,
              "line mute settle must fit into the amp off delay");

#if APP_CONTROLLER_DEBUG
#define APP_LOG(...) Serial.printf("[APP] " __VA_ARGS__)
#else
#define APP_LOG(...) \
    do {             \
    } while (0)
#endif

namespace {

// ---------------------------------------------------------------------------
// Стан контролера (єдиний письменник — цей файл, під s_lock)
// ---------------------------------------------------------------------------
// [Prompt 47a] Фаза Ramp прибрана: плавність гучності робить audio/dsp_volume.
enum class Phase : uint8_t {
    None,    // переходу немає
    Settle,  // мʼют, чекаємо kUnmuteDelayMs (і прогрів підсилювача), потім розмʼют
};

#if LINE_PATH_ENABLE
// [Prompt 47c] Автомат проходу зовнішніх входів (див. заголовок файлу).
enum class LineState : uint8_t { Off, Starting, Active, Switching, Releasing, Failed };
enum class LineStartStep : uint8_t { WaitIdle, WaitRetry, WaitRunning };
// Що робити після звільнення ресурсів, якщо пристрій «увімкнений» і вхід зовнішній.
enum class LineAfter : uint8_t { Auto, Retry, Fail };
#endif

SemaphoreHandle_t s_lock = nullptr;
bool s_started = false;
Mode s_mode = Mode::Standby;
uint8_t s_input = 0;
uint16_t s_station = 0;
int8_t s_volume = 0;  // ЦІЛЬОВА гучність
bool s_userMute = false;
// [Prompt 21b/46a] Гучності профілів усіх логічних входів (включно з входом 3, якого PT2313L не має).
// Пресет EQ у контролері НЕ дублюється (живе лише в Settings::profiles[i].eqGainsDb).
int8_t s_volumes[defaults::kInputCount] = {};
AdjustTarget s_target = AdjustTarget::Volume;  // [Prompt 46a] завжди Volume (поле AppState.adjustTarget)
// [Prompt 32] Подія «показати спливне вікно параметра»: що показувати і лічильник подій.
// Лічильник росте лише на ЛОКАЛЬНІ зміни (handleLocked); веб-команди (runWebCommand) його не чіпають.
AdjustTarget s_popupTarget = AdjustTarget::Volume;
uint8_t s_popupSeq = 0;
MenuContext s_menuCtx = MenuContext::None;
uint16_t s_menuSel = 0;

Phase s_phase = Phase::None;
uint32_t s_phaseStartMs = 0;

bool s_hwMute = false;  // [P48a] цифровий мʼют виставлено (= desiredMute() на момент останнього applyHardwareMute())

// [Prompt 47a] Пауза радіо: DSP-мʼют тримається, поки не буде наступного startStream() (див. pauseRadio()).
bool s_stopHold = false;

#if LINE_PATH_ENABLE
// --- [Prompt 47c] Прохід зовнішніх входів ---
LineState s_lineState = LineState::Off;
LineStartStep s_lineStep = LineStartStep::WaitIdle;
LineAfter s_lineAfter = LineAfter::Auto;
bool s_lineRetryUsed = false;     // автоповтор після невдачі вже витрачено
bool s_lineApplied = false;       // вхід чіпа + гучність + EQ уже надіслано для поточної спроби старту
bool s_lineMuteSent = true;       // останній setUserMute(), надісланий у LinePath (LinePath стартує в мʼюті)
uint8_t s_lineSwitchStep = 0;     // Switching: 0 = чекаємо мʼют, 1 = чекаємо стабілізацію входу
uint8_t s_lineRelStep = 0;        // Releasing: 0 = чекаємо мʼют, 1 = чекаємо звільнення
bool s_lineRelCalled = false;     // returnToRadio() прийнято
uint32_t s_lineT0 = 0;            // початок поточного кроку (WaitRetry: момент повтору)
uint32_t s_lineRelayMs = 0;       // [P48a] коли востаннє виставляли реле-селектор
#endif

uint32_t s_lastSyncMs = 0;

// true: потік потрібен, але Wi-Fi ще немає; playUrl() відкладено до зʼєднання.
bool s_playPending = false;

// [Prompt 25] Що саме запущено: URL потоку, який прийняв AudioPlayer, і застосований рівень, дБ.
char s_playUrl[sizeof(Station::url)] = {};
bool s_playUrlValid = false;
int8_t s_levelDb = static_cast<int8_t>(station_level_cfg::kDefaultStationLevelDb);
bool s_persistNeeded = false;

// --- [Prompt 15] Навчання IR ---
Mode s_irPrevMode = Mode::Radio;  // режим ДО навчання; осмислений лише при s_mode == IrLearn
bool s_irHolding = false;         // фінальний результат (Success/Timeout) вже показується
uint32_t s_irHoldUntilMs = 0;
// Остання опублікована в AppState трійка (щоб не смикати мʼютекс стану щотакту).
IrLearnStatus s_irPubStatus = IrLearnStatus::Idle;
Action s_irPubTarget = Action::POWER;
Action s_irPubOther = Action::POWER;

// --- [Prompt 28] Офлайн-режим і «тихий» перезапуск ---
bool s_offline = false;  // Wi-Fi вимкнено, вхід Radio недоступний (до перезапуску)

// --- [Prompt 39] Маска входів, дозволених користувачем (дзеркало Settings::inputEnabledMask) ---
// Біт i = вхід i; біт 0 (радіо) завжди 1. Завантажується в begin(), міняється лише веб-командою
// InputsEnabledSet (під s_lock). Апаратну доступність входу цей прапорець не замінює (див. inputAvailable()).
uint8_t s_inputMask = settings_cfg::kInputMaskAll;

enum class RestartPhase : uint8_t {
    None,      // перезапуску немає
    WaitAmp,   // мʼют зроблено, чекаємо LOW піна підсилювача (ampBusy() == false)
    Pause,     // пін LOW, неблокуюча пауза kRestartMuteMs
};
RestartPhase s_restartPhase = RestartPhase::None;
uint32_t s_restartDeadlineMs = 0;  // страховка: після цього моменту не чекаємо пін
uint32_t s_restartAtMs = 0;        // момент ESP.restart() (фаза Pause)

// --- [Prompt 16] OTA ---
Mode s_otaPrevMode = Mode::Radio;         // режим ДО OTA; осмислений лише при s_mode == OtaUpdate
volatile uint8_t s_otaPubPercent = 0;     // остання опублікована цифра (читає setOtaProgress без мʼютекса)
volatile bool s_otaAbortPending = false;  // otaFailed() не взяв мʼютекс -> tickLocked() відновить

#if FEATURE_AMP_STANDBY
// --- [Prompt 23c] Standby підсилювача ---
bool s_ampWanted = false;         // true між powerOnTransition() і enterStandby()
bool s_ampWaking = false;         // пін піднято, іде прогрів kAmpWakeMs
uint32_t s_ampWakeUntilMs = 0;
uint32_t s_ampOffAtMs = 0;        // найраніший момент пониження піна
uint32_t s_ampOffForceAtMs = 0;   // після нього — LOW навіть без підтвердженого мʼюту
#endif

// [Prompt 47a] Реалізація нижче (потрібна startStream(), що визначена раніше за desiredMute()).
void syncRadioMute(bool waitSilent);

// ---------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------
inline bool reached(uint32_t now, uint32_t t) {
    return static_cast<int32_t>(now - t) >= 0;
}

// [Prompt 28] Іде «тихий» перезапуск: звук заблоковано, події ігноруються.
inline bool restarting() {
    return s_restartPhase != RestartPhase::None;
}

inline int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Standby:       return "Standby";
        case Mode::Radio:         return "Radio";
        case Mode::ExternalInput: return "ExternalInput";
        case Mode::Menu:          return "Menu";
        case Mode::IrLearn:       return "IrLearn";
        case Mode::WifiSetup:     return "WifiSetup";
        case Mode::OtaUpdate:     return "OtaUpdate";  // [Prompt 16]
    }
    return "?";
}

#if APP_CONTROLLER_LOG_EVENTS && APP_CONTROLLER_DEBUG
const char* actionName(Action a) {
    switch (a) {
        case Action::POWER:        return "POWER";
        case Action::UP:           return "UP";
        case Action::DOWN:         return "DOWN";
        case Action::LEFT:         return "LEFT";
        case Action::RIGHT:        return "RIGHT";
        case Action::OK:           return "OK";
        case Action::ENC_CW:       return "ENC_CW";
        case Action::ENC_CCW:      return "ENC_CCW";
        case Action::ENC_PRESS:    return "ENC_PRESS";
        case Action::VOL_UP:       return "VOL_UP";
        case Action::VOL_DOWN:     return "VOL_DOWN";
        case Action::MUTE:         return "MUTE";
        case Action::MENU:         return "MENU";
        case Action::BACK:         return "BACK";
        case Action::INPUT_RADIO:  return "INPUT_RADIO";
        case Action::INPUT_TV:     return "INPUT_TV";
        case Action::INPUT_PC:     return "INPUT_PC";
        case Action::INPUT_AUX:    return "INPUT_AUX";
        case Action::DIGIT_0:      return "DIGIT_0";
        case Action::DIGIT_1:      return "DIGIT_1";
        case Action::DIGIT_2:      return "DIGIT_2";
        case Action::DIGIT_3:      return "DIGIT_3";
        case Action::DIGIT_4:      return "DIGIT_4";
        case Action::DIGIT_5:      return "DIGIT_5";
        case Action::DIGIT_6:      return "DIGIT_6";
        case Action::DIGIT_7:      return "DIGIT_7";
        case Action::DIGIT_8:      return "DIGIT_8";
        case Action::DIGIT_9:      return "DIGIT_9";
        case Action::BASS_UP:      return "BASS_UP";
        case Action::BASS_DOWN:    return "BASS_DOWN";
        case Action::TREBLE_UP:    return "TREBLE_UP";
        case Action::TREBLE_DOWN:  return "TREBLE_DOWN";
        case Action::BALANCE_UP:   return "BALANCE_UP";
        case Action::BALANCE_DOWN: return "BALANCE_DOWN";
        case Action::GAIN_UP:      return "GAIN_UP";
        case Action::GAIN_DOWN:    return "GAIN_DOWN";
    }
    return "?";
}

const char* sourceName(EventSource s) {
    switch (s) {
        case EventSource::BUTTON:  return "BUTTON";
        case EventSource::ENCODER: return "ENCODER";
        case EventSource::IR:      return "IR";
        case EventSource::WEB:     return "WEB";
    }
    return "?";
}
#endif

// --- Станції ---------------------------------------------------------------
// [Prompt 11] Список живе в StationStore (LittleFS + PSRAM). Station (≈258 байт)
// завжди локальна й короткоживуча (стек задачі app_ctrl).
uint16_t stationCount() {
    const size_t n = StationStore::count();
    return static_cast<uint16_t>(n > 0xFFFFu ? 0xFFFFu : n);
}

// [Prompt 14] Обрізає s_station до [0, count-1]. true — індекс було змінено.
// count == 0 не чіпаємо: порожній список обробляють startStream()/openStationList().
// Також обережно з count() == 0 через тайм-аут мʼютекса — тоді теж не чіпаємо.
bool clampStationIndex() {
    const uint16_t n = stationCount();
    if (n == 0 || s_station < n) {
        return false;
    }
    APP_LOG("station index %u out of range (%u), clamped to %u\n",
            static_cast<unsigned>(s_station), static_cast<unsigned>(n),
            static_cast<unsigned>(n - 1));
    s_station = static_cast<uint16_t>(n - 1);
    s_persistNeeded = true;
    return true;
}

// Копіює назву станції i в out; для недійсного індексу — порожній рядок.
void stationLabel(uint16_t i, char* out, size_t cap) {
    if (cap == 0) {
        return;
    }
    Station st;
    if (StationStore::get(i, st)) {
        strlcpy(out, st.name, cap);
    } else {
        out[0] = '\0';
    }
}

// [Prompt 20b] ЄДИНЕ місце вирішення назви поточної станції для AppState.stationName.
// Пріоритет: назва зі списку для s_station; якщо її немає (порожній список, індекс поза
// межами, порожня назва) — ICY-назва потоку (icyName, може бути nullptr); інакше "".
// Викликати лише з задачі контролера під s_lock (StationStore бере власний короткий мʼютекс;
// з колбека аудіо до нього звертатись не можна — тут це не так).
void resolveStationName(const char* icyName, char* out, size_t cap) {
    if (cap == 0) {
        return;
    }
    stationLabel(s_station, out, cap);
    if (out[0] == '\0' && icyName != nullptr) {
        strlcpy(out, icyName, cap);
    }
}

// [Prompt 23b] ЄДИНЕ місце вирішення назви входу для AppState.inputName.
// Пріоритет: Settings::inputNames[idx]; порожня назва чи недійсний індекс -> defaults::kInputNames[idx]
// (лише для idx < kInputCount); інакше "". Буфер out повністю обнуляється, а потім заповнюється
// (однакові назви -> однакові байти). Викликати лише з задачі контролера: бере мʼютекс Settings
// через SettingsStore::snapshot() (короткий; те саме робить leaveStandby()).
static_assert(sizeof(Settings::inputNames[0]) == kInputNameMax,
              "AppStateData::inputName must match Settings::inputNames[i] size");

void resolveInputName(uint8_t idx, char* out, size_t cap) {
    if (cap == 0) {
        return;
    }
    memset(out, 0, cap);
    if (idx >= defaults::kInputCount) {
        return;
    }
    const Settings st = SettingsStore::snapshot();
    strlcpy(out, st.inputNames[idx], cap);
    if (out[0] == '\0') {
        strlcpy(out, defaults::kInputNames[idx], cap);
    }
}

// --- Потік і Wi-Fi ---------------------------------------------------------
// AudioPlayer сам Wi-Fi не перевіряє, а без піднятого мережевого стеку
// бібліотека падає в assert (xQueueSemaphoreTake). Тому playUrl() викликаємо
// лише при підключеному Wi-Fi. [Prompt 12] Джерело — WifiManager.
bool wifiUp() {
    return WifiManager::isConnected();
}

// [Prompt 25] Рівень станції в межах kLevelMinDb..kLevelMaxDb (файл/імпорт/веб уже валідують,
// тут — страховка від пошкоджених даних).
int8_t clampLevelDb(int v) {
    if (v < station_level_cfg::kLevelMinDb) v = station_level_cfg::kLevelMinDb;
    if (v > station_level_cfg::kLevelMaxDb) v = station_level_cfg::kLevelMaxDb;
    return static_cast<int8_t>(v);
}

// [Prompt 25] Виставляє рівень виходу декодера й запамʼятовує його. Один рядок логу на
// застосування (без спаму: на старті станції й при реальній живій зміні).
void applyStationLevel(int8_t db, uint16_t idx, const char* why) {
    AudioPlayer::setOutputTrimDb(db);
    s_levelDb = db;
    APP_LOG("station level %d dB (#%u, %s)\n", static_cast<int>(db), static_cast<unsigned>(idx),
            why);
}

void startStream() {
    // [Prompt 47c] Радіо-потік лише на вході 0: на зовнішньому вході його запуск зіпсував би прохід
    // (мультиплексор GPIO18, 48 кГц на I2S0). Усі радіо-команди й так гейтяться за s_input.
    if (s_input != 0) {
        s_playPending = false;
        APP_LOG("stream start blocked (external input)\n");
        return;
    }
    // [Prompt 47a] Новий запуск знімає утримання мʼюту після паузи (DSP знову керується desiredMute()).
    if (s_stopHold) {
        s_stopHold = false;
        syncRadioMute(false);
    }
    // [Prompt 28] ЄДИНА точка, що блокує запуск потоку в офлайні (і під час перезапуску): усі
    // шляхи (старт, вибір станції, play/pause, відкладений старт, веб) проходять через неї.
    if (s_offline || restarting()) {
        s_playPending = false;
        APP_LOG("stream start blocked (%s)\n", s_offline ? "offline" : "restarting");
        return;
    }
    if (stationCount() == 0) {
        s_playPending = false;
        applyStationLevel(static_cast<int8_t>(station_level_cfg::kDefaultStationLevelDb),
                          s_station, "no stations");
        APP_LOG("no stations: nothing to play\n");
        return;
    }
    clampStationIndex();  // [Prompt 14] список міг скоротитись з вебу
    if (wifiUp()) {
        s_playPending = false;
        Station st;
        if (!StationStore::get(s_station, st)) {
            applyStationLevel(static_cast<int8_t>(station_level_cfg::kDefaultStationLevelDb),
                              s_station, "station not found");
            APP_LOG("station %u not found\n", static_cast<unsigned>(s_station));
            return;
        }
        // [Prompt 25] Рівень станції — ДО playUrl(): XSMT замʼючується в doPlay(), а перший
        // новий блок прийде лише після підключення, тож звук стартує вже з цим рівнем.
        const int8_t prevLevel = s_levelDb;
        applyStationLevel(clampLevelDb(st.levelDb), s_station, "start");
        if (AudioPlayer::playUrl(st.url)) {
            strlcpy(s_playUrl, st.url, sizeof(s_playUrl));
            s_playUrlValid = true;
        } else {
            APP_LOG("playUrl rejected\n");
            // Потік не змінився -> повертаємо рівень попереднього (якщо грав) без зайвого логу.
            AudioPlayer::setOutputTrimDb(prevLevel);
            s_levelDb = prevLevel;
        }
    } else {
        s_playPending = true;
        APP_LOG("no Wi-Fi: play deferred\n");
    }
}

void stopStream() {
    s_playPending = false;
    s_playUrlValid = false;  // [Prompt 25]
    AudioPlayer::stop();
}

// [Prompt 46a] Єдина підтримувана ціль регулювання — гучність (бас/дискант/баланс/gain прибрано).
bool targetSupported(AdjustTarget t) {
    return t == AdjustTarget::Volume;
}

// --- [Prompt 21b/46a] Профілі входів -------------------------------------------
// Робоча гучність поточного входу -> s_volumes[s_input].
void captureActiveProfile() {
    if (s_input >= defaults::kInputCount) {
        return;
    }
    s_volumes[s_input] = s_volume;
}

// [Prompt 47a] Цільова гучність -> цифрове послаблення радіо. У мʼюті (Settle, standby, ...) слух не
// змінюється: DSP тримає ціль 0, нове послаблення вступить у силу після розмʼюту.
void applyRadioVolume() {
    radio_volume::setAttenuationDb(audio_cfg::radioVolumeDb(s_volume));
}

// [Prompt 47c] Гучність зовнішніх входів: та сама крива, що в радіо, + kExtLevelTrimDb (LinePath обрізає до свого діапазону).
float extVolumeDb(int vol) {
    return audio_cfg::radioVolumeDb(vol) + cfg::kExtLevelTrimDb;
}

// [Prompt 47c] Цільова гучність -> LinePath (діє й до старту проходу; плавно через DspVolume).
void lineApplyVolume() {
#if LINE_PATH_ENABLE
    if (LinePath::isReady()) {
        LinePath::setVolumeDb(extVolumeDb(s_volume));
    }
#endif
}

// [Prompt 47c] Живе застосування гучності активного шляху: радіо (s_input == 0) або прохід.
void applyActiveVolume() {
    if (s_input == 0) {
        applyRadioVolume();
    } else {
        lineApplyVolume();
    }
}

// s_volumes[idx] -> робоча гучність, обрізана за capabilities() поточного чипа.
// Чипа не торкається й AppState не публікує (це робить викликач). [P47a] Оновлює цифрову гучність радіо.
void loadActiveFromProfile(uint8_t idx) {
    if (idx >= defaults::kInputCount) {
        idx = 0;
    }
    s_volume = static_cast<int8_t>(clampInt(s_volumes[idx], audio_cfg::kVolumeUiMin, audio_cfg::kVolumeUiMax));
    // [Prompt 47c] Радіо — цифрова гучність одразу; для зовнішнього входу значення піде в LinePath разом з вибором
    // входу чіпа (lineApplyParams()), щоб не міняти звук старого входу, поки він ще згасає.
    if (idx == 0) {
        applyRadioVolume();
    }
}

// [Prompt 46a] Екземпляр еквалайзера радіо = пресет входу 0 (Settings::profiles[0].eqGainsDb).
// Викликається, коли радіо знову стає активним (changeInput на 0, вмикання на вході 0): якщо в екземплярі
// вже той самий пресет — нічого не робить. На інших входах екземпляр радіо не чіпається.
void applyRadioEq() {
    const Settings st = SettingsStore::snapshot();
    int8_t have[eq_cfg::kBandCount];
    eq::getAllDb(have);
    if (memcmp(have, st.profiles[0].eqGainsDb, sizeof(have)) != 0) {
        eq::setAllDb(st.profiles[0].eqGainsDb);
        APP_LOG("radio EQ preset applied (input 0 profile)\n");
    }
}

// --- Мʼют ----------------------------------------------------------------------
// [Prompt 15] «Логічний» Standby: Standby АБО навчання IR, розпочате зі Standby. Без цього
// вхід у IrLearn зі Standby зняв би мʼют атенюаторів (desiredMute() бачив би
// mode != Standby) і зовнішній вхід заграв би на «вимкненому» пристрої.
bool logicallyStandby() {
    return s_mode == Mode::Standby ||
           (s_mode == Mode::IrLearn && s_irPrevMode == Mode::Standby);
}

bool desiredMute() {
    return s_userMute || logicallyStandby() || s_phase == Phase::Settle ||
           s_mode == Mode::OtaUpdate ||  // [Prompt 16]: під час прошивки тиша
           restarting() ||               // [Prompt 28]: перезапуск — тиша до кінця
           s_mode == Mode::WifiSetup;    // [Prompt 28]: екран налаштування Wi-Fi — тиша
}

// [Prompt 47c] Мʼют проходу: стан користувача/переходів + будь-який стан проходу, крім Active (старт, перемикання,
// звільнення, помилка). Внутрішніх мʼютів line_path (Stall тощо) не чіпаємо.
#if LINE_PATH_ENABLE
bool lineMuteWanted() {
    return desiredMute() || s_lineState != LineState::Active;
}

// Надсилає в LinePath актуальний мʼют користувача (лише при зміні). Мʼют плавний (DspVolume).
void syncLineMute() {
    if (!LinePath::isReady()) {
        return;
    }
    const bool want = lineMuteWanted();
    if (want == s_lineMuteSent) {
        return;
    }
    LinePath::setUserMute(want);
    s_lineMuteSent = want;
    APP_LOG("line user mute %s\n", want ? "on" : "off");
}
#else
inline void syncLineMute() {}
#endif

// [Prompt 47a] Чекає, поки цифровий мʼют радіо дозгасне. Лише коли радіо реально грає (інакше блоки
// хука не йдуть і згасати нічому): тоді просто просимо миттєвий мʼют для найближчого блоку.
// Блокуюче (vTaskDelay(1)) до kRadioMuteWaitMs; після таймауту — примусовий мʼют.
void radioFadeWait() {
    const uint32_t t0 = millis();
    while (!radio_volume::isSilent()) {
        if (static_cast<uint32_t>(millis() - t0) >= cfg::kRadioMuteWaitMs) {
            radio_volume::requestForceMute();
            APP_LOG("radio mute fade timeout, forced\n");
            return;
        }
        vTaskDelay(1);
    }
}

// [Prompt 47a] Приводить цифровий мʼют радіо у відповідність до desiredMute() (або утримання паузи).
// waitSilent=true — дочекатись згасання ПЕРЕД наступним кроком (зупинка плеєра, зміна станції, вимкнення
// підсилювача). Розмʼют — setMute(false): DSP сам плавно підніме звук з першого блоку.
void syncRadioMute(bool waitSilent) {
    const bool want = desiredMute() || s_stopHold;
    if (!want) {
        radio_volume::setMute(false);
        return;
    }
    radio_volume::setMute(true);
    const PlayerState ps = AudioPlayer::state();
    if (ps != PlayerState::Playing && ps != PlayerState::Buffering) {
        radio_volume::requestForceMute();  // звуку немає: наступний потік почнеться з тиші
        return;
    }
    if (waitSilent && !radio_volume::isSilent()) {
        radioFadeWait();
    }
}

// Мʼют: цифровий (радіо) + цифровий мʼют проходу зовнішніх входів. [P48a] Апаратного (чип) мʼюту немає:
// s_hwMute = desiredMute() (піну підсилювача P23c достатньо цього «підтвердження»: DSP радіо / прохід уже
// отримали запит на мʼют, а перед вимкненням підсилювача контролер чекає згасання через waitSilent).
void applyHardwareMute(bool waitSilent = false) {
    syncRadioMute(waitSilent);  // [Prompt 47a]
    syncLineMute();             // [Prompt 47c] у режимі радіо LinePath і так у мʼюті (стан != Active)
    const bool want = desiredMute();
    if (want != s_hwMute) {
        s_hwMute = want;
        APP_LOG("mute %s\n", want ? "on" : "off");
    }
}

// [Prompt 47a] Пауза радіо: плавний мʼют (чекаємо згасання) -> stop() плеєра. DSP лишається замʼюченим
// (s_stopHold) до наступного startStream(): stop() асинхронний, і розмʼют до фактичної зупинки дав би
// «виринання» старого потоку. Наступний запуск потоку починається з тиші й плавно нарощується.
void pauseRadio() {
    s_stopHold = true;
    syncRadioMute(true);
    stopStream();
}

// --- [Prompt 23c] Standby підсилювача -----------------------------------------
#if FEATURE_AMP_STANDBY
// Єдиний рушій піна: викликається з tickLocked() (і одразу з ampRequestOn()).
void ampService(uint32_t now) {
    if (s_ampWanted) {
        if (!AmpStandby::isRunning()) {
            // Вмикаємо лише під ПІДТВЕРДЖЕНИМ мʼютом (s_hwMute = востаннє успішно надіслано).
            if (s_hwMute) {
                AmpStandby::set(true);
                s_ampWaking = true;
                s_ampWakeUntilMs = now + amp_cfg::kAmpWakeMs;
                APP_LOG("amp on, warm-up %u ms\n", static_cast<unsigned>(amp_cfg::kAmpWakeMs));
            }
        } else if (s_ampWaking && reached(now, s_ampWakeUntilMs)) {
            s_ampWaking = false;
            APP_LOG("amp warm-up done\n");
        }
        return;
    }
    s_ampWaking = false;
    if (AmpStandby::isRunning() && reached(now, s_ampOffAtMs) &&
        (s_hwMute || reached(now, s_ampOffForceAtMs))) {
        AmpStandby::set(false);
        APP_LOG("amp standby%s\n", s_hwMute ? "" : " (mute not confirmed, forced)");
    }
}

// Потрібен ПІСЛЯ startTransition() (мʼют уже надіслано). Повторне вмикання, поки пін ще
// HIGH, скасовує вимкнення без прогріву.
void ampRequestOn() {
    s_ampWanted = true;
    ampService(millis());
}

// Потрібен ПІСЛЯ applyHardwareMute() у enterStandby(); пін опуститься в tick().
void ampRequestOff() {
    const uint32_t now = millis();
    s_ampWanted = false;
    s_ampWaking = false;
    s_ampOffAtMs = now + amp_cfg::kAmpOffDelayMs;
    s_ampOffForceAtMs = now + amp_cfg::kAmpOffMuteWaitMs;
}

// Settle може завершитись (розмʼют), лише коли підсилювач не потрібен або прогрітий.
bool ampReady() {
    return !s_ampWanted || (AmpStandby::isRunning() && !s_ampWaking);
}

// Чекаємо на перехід піна: задача опитує чергу з активним періодом.
bool ampBusy() {
    return s_ampWanted ? (!AmpStandby::isRunning() || s_ampWaking) : AmpStandby::isRunning();
}
#else
inline void ampService(uint32_t) {}
inline void ampRequestOn() {}
inline void ampRequestOff() {}
inline bool ampReady() { return true; }
inline bool ampBusy() { return false; }
#endif

// --- Публікація в AppState / Settings --------------------------------------
struct PubCtx {
    Mode mode;
    uint8_t input;
    int8_t volume;
    bool mute;
    uint16_t station;
    AdjustTarget target;
    MenuContext menuCtx;
    uint16_t menuSel;
    bool offline;     // [Prompt 28]
    bool restarting;  // [Prompt 28]
    AdjustTarget popupTarget;  // [Prompt 32]
    uint8_t popupSeq;          // [Prompt 32]
    uint8_t inputEnabledMask;  // [Prompt 39]
    char inputName[kInputNameMax];  // [Prompt 23b]
};

void applyPub(AppStateData& s, void* c) {
    const PubCtx* p = static_cast<const PubCtx*>(c);
    s.mode = p->mode;
    s.inputIndex = p->input;
    s.volume = p->volume;
    // [Prompt 46a] Застарілі поля: тембр/баланс/gain/loudness більше не регулюються — завжди нейтральні
    // константи (екрани й веб компілюються й не ламаються; поля видалить P49/P50).
    s.bass = cfg::kLegacyToneValue;
    s.treble = cfg::kLegacyToneValue;
    s.balance = cfg::kLegacyToneValue;  // [P48a] баланс (dB) у AppState з'явиться в P48b
    s.gain = cfg::kLegacyToneValue;
    s.loudness = cfg::kLegacyLoudness;
    s.mute = p->mute;
    s.stationIndex = p->station;
    s.adjustTarget = p->target;
    s.menuContext = p->menuCtx;
    s.menuSelection = p->menuSel;
    s.offline = p->offline;        // [Prompt 28]
    s.restarting = p->restarting;  // [Prompt 28]
    s.popupTarget = p->popupTarget;  // [Prompt 32]
    s.popupSeq = p->popupSeq;        // [Prompt 32]
    s.inputEnabledMask = p->inputEnabledMask;  // [Prompt 39]
    memcpy(s.inputName, p->inputName, sizeof(s.inputName));  // [Prompt 23b]
}

void publishState() {
    PubCtx p = {s_mode,        s_input,   s_volume,  s_userMute, s_station,
                s_target,      s_menuCtx, s_menuSel, s_offline,  restarting(),
                s_popupTarget, s_popupSeq, s_inputMask};
    resolveInputName(s_input, p.inputName, sizeof(p.inputName));  // [Prompt 23b]
    AppState::modify(applyPub, &p);
}

// [Prompt 21b/46a] Єдине місце запису в Settings: lastInput/lastStation/lastMute + ГУЧНОСТІ всіх профілів
// входів (гучність поточного входу перед цим оновлюється з робочого значення). Пресети EQ профілів
// сюди НЕ входять: їх пише лише веб-обробник /api/eq.
struct PersistCtx {
    uint8_t input;
    uint16_t station;
    bool mute;
    const int8_t* volumes;
};

static_assert(sizeof(s_volumes) / sizeof(s_volumes[0]) ==
                  sizeof(Settings::profiles) / sizeof(Settings::profiles[0]),
              "controller volume table must have one entry per Settings profile");

void applyPersist(Settings& s, void* c) {
    const PersistCtx* p = static_cast<const PersistCtx*>(c);
    s.lastInput = p->input;
    s.lastStation = p->station;
    s.lastMute = p->mute;
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        s.profiles[i].volume = p->volumes[i];  // eqGainsDb профілів не чіпаємо
    }
}

// SettingsStore::modify() сама позначає кеш «брудним» (як requestSave()):
// фактичний запис відбудеться з дебаунсом (не на кожен крок енкодера).
void persist() {
    captureActiveProfile();
    PersistCtx p = {s_input, s_station, s_userMute, s_volumes};
    SettingsStore::modify(applyPersist, &p);
}

// [Prompt 39] Запис маски входів у Settings (окремо від persist(): маску пише лише команда вебу,
// а applyPersist() її не чіпає). SettingsStore::modify() — NVS з дебаунсом.
void applyMaskPersist(Settings& s, void* c) {
    s.inputEnabledMask = *static_cast<const uint8_t*>(c);
}

// [Prompt 20b] Негайне оновлення лише назви станції (без решти полів syncPlayer()), щоб після
// вибору станції/входу дисплей і веб не чекали на найближчий період синхронізації.
// Поза Radio (чи в логічному Standby) назва порожня — як і в syncPlayer().
struct NameCtx {
    char name[64];
};

void applyName(AppStateData& s, void* c) {
    const NameCtx* x = static_cast<const NameCtx*>(c);
    strlcpy(s.stationName, x->name, sizeof(s.stationName));
}

void publishStationName() {
    NameCtx c = {};
    if (s_input == 0 && !logicallyStandby()) {
        char icy[sizeof(c.name)];
        AudioPlayer::currentMetadata(icy, sizeof(icy), nullptr, 0);
        resolveStationName(icy, c.name, sizeof(c.name));
    }
    AppState::modify(applyName, &c);
}

struct SyncCtx {
    char station[64];
    char title[128];
    bool playing;
    bool wifi;
    StreamStatus status;  // [Prompt 10] ДОДАНО: детальний статус поток
    // [Prompt 12] ДОДАНО: дані WifiManager для екрана WifiSetup.
    char wifiSsid[33];
    char wifiIp[16];
    bool wifiApMode;
    char inputName[kInputNameMax];  // [Prompt 23b]: назва могла змінитись з вебу
    int8_t wifiRssi;                // [Prompt 36]: квантований RSSI STA (0 = невідомо)
};

void applySync(AppStateData& s, void* c) {
    const SyncCtx* x = static_cast<const SyncCtx*>(c);
    strlcpy(s.stationName, x->station, sizeof(s.stationName));
    strlcpy(s.trackTitle, x->title, sizeof(s.trackTitle));
    s.streamPlaying = x->playing;
    s.wifiConnected = x->wifi;
    s.streamStatus = x->status;  // [Prompt 10] ДОДАНО
    // [Prompt 12] memcpy усього буфера (хвіст обнулено в copyInfo): однакові рядки
    // дають однакові байти, тож screens не перемальовує екран без потреби.
    memcpy(s.wifiSsid, x->wifiSsid, sizeof(s.wifiSsid));
    memcpy(s.wifiIp, x->wifiIp, sizeof(s.wifiIp));
    s.wifiApMode = x->wifiApMode;
    memcpy(s.inputName, x->inputName, sizeof(s.inputName));  // [Prompt 23b]
    s.wifiRssi = x->wifiRssi;  // [Prompt 36]
}

// [Prompt 36] RSSI (дБм) -> значення для AppState: 0, якщо невідомо (>= 0), інакше обрізано знизу й
// округлено до кратного kRssiQuantDb. Від'ємний вхід ніколи не дає 0.
int8_t quantizeRssi(int32_t rssi) {
    if (rssi >= 0) {
        return 0;
    }
    if (rssi < wifi_status_cfg::kRssiFloorDb) {
        rssi = wifi_status_cfg::kRssiFloorDb;
    }
    const int32_t step = wifi_status_cfg::kRssiQuantDb;
    return static_cast<int8_t>(-(((-rssi) + step / 2) / step) * step);
}

// [Prompt 25] Жива зміна рівня станції, що грає (редагування з вебу). Викликається з
// періодичного блоку tickLocked() під s_lock. Застосовуємо лише якщо: вхід Radio, не standby,
// потік не зупинено, і станція за s_station — та сама (URL збігається з URL потоку). Якщо
// URL інший (move/remove зсунули індекс, URL відредаговано) — рівень лишається рівнем потоку.
void syncStationLevel() {
    if (s_input != 0 || logicallyStandby() || !s_playUrlValid || s_playPending) {
        return;
    }
    if (AudioPlayer::state() == PlayerState::Idle) {
        return;
    }
    Station st;
    if (!StationStore::get(s_station, st)) {
        return;
    }
    if (strcmp(st.url, s_playUrl) != 0) {
        return;
    }
    const int8_t want = clampLevelDb(st.levelDb);
    if (want != s_levelDb) {
        applyStationLevel(want, s_station, "live");
    }
}

void syncPlayer() {
    SyncCtx c = {};
    if (s_input == 0 && !logicallyStandby()) {  // [Prompt 15]: було s_mode != Standby
        // [Prompt 20b] ICY-назва — лише запасний варіант; пріоритет має назва зі списку.
        char icy[sizeof(c.station)];
        AudioPlayer::currentMetadata(icy, sizeof(icy), c.title, sizeof(c.title));
        resolveStationName(icy, c.station, sizeof(c.station));
    }
    c.playing = AudioPlayer::isPlaying();
    c.wifi = wifiUp();
    // [Prompt 12] ДОДАНО
    WifiManager::copyInfo(c.wifiSsid, sizeof(c.wifiSsid), c.wifiIp, sizeof(c.wifiIp));
    c.wifiApMode = WifiManager::isApMode();
    c.wifiRssi = c.wifi ? quantizeRssi(WifiManager::rssi()) : 0;  // [Prompt 36]
    resolveInputName(s_input, c.inputName, sizeof(c.inputName));  // [Prompt 23b]

    // [Prompt 10] ДОДАНО: маппінг PlayerState → StreamStatus за назвою
    // audio_player.h::PlayerState і core/app_state.h::StreamStatus мають однакові
    // стани, але різний порядок значень, тому маппуємо явно для безпеки й ясності.
    // Це дозволяє ui/screens розрізнити Buffering/Error/Reconnecting без
    // включення audio/audio_player.h.
    const PlayerState playerState = AudioPlayer::state();
    switch (playerState) {
        case PlayerState::Idle:         c.status = StreamStatus::Idle; break;
        case PlayerState::Connecting:   c.status = StreamStatus::Connecting; break;
        case PlayerState::Buffering:    c.status = StreamStatus::Buffering; break;
        case PlayerState::Playing:      c.status = StreamStatus::Playing; break;
        case PlayerState::Error:        c.status = StreamStatus::Error; break;
        case PlayerState::Reconnecting: c.status = StreamStatus::Reconnecting; break;
        default:                        c.status = StreamStatus::Idle; break;
    }
    
    AppState::modify(applySync, &c);
}

// --- Гучність ----------------------------------------------------------------
// [Prompt 47a] Змінює ЦІЛЬОВУ гучність і одразу застосовує цифрове послаблення радіо. Плавність
// (100 мс) забезпечує DSP; у мʼюті/Settle нове значення просто чекає на розмʼют. Чип не чіпаємо.
bool setVolumeTarget(int v) {
    v = clampInt(v, audio_cfg::kVolumeUiMin, audio_cfg::kVolumeUiMax);
    if (v == s_volume) {
        return false;
    }
    s_volume = static_cast<int8_t>(v);
    applyActiveVolume();  // [Prompt 47c] радіо або прохід
    return true;
}

// Початок переходу: мʼют ПЕРШИМ (цифровий радіо з очікуванням згасання, потім чип). Розмʼют — у tick()
// після kUnmuteDelayMs і прогріву підсилювача. Якщо перехід уже йде — просто перезапускає таймер.
void startTransition() {
    s_phase = Phase::Settle;
    s_phaseStartMs = millis();
    applyHardwareMute(true);
}

// ---------------------------------------------------------------------------
// [Prompt 47c] Маршрутизація входу: радіо або прохід зовнішніх входів
// ---------------------------------------------------------------------------
// [Prompt 48a] Реле-селектор для s_input: 1 -> TV Box, 2 -> Computer. Радіо (0) реле НЕ чіпає. Викликати лише
// коли прохід цифрово замʼючений або не працює (клацання реле не має потрапити в динаміки). Момент перемикання
// запамʼятовується: розмʼют не раніше input_selector_cfg::kRelaySettleMs.
void applyRelayForInput() {
    if (s_input != 0) {
        input_selector::select(s_input);
    }
#if LINE_PATH_ENABLE
    s_lineRelayMs = millis();
#endif
}

// Запуск радіо: пресет EQ входу 0, потік (профіль входу 0 уже в s_volume / radio_volume). Реле не чіпаємо.
void radioBegin() {
    applyRadioEq();  // [Prompt 46a]
    startStream();
}

#if LINE_PATH_ENABLE
inline bool lineBusy() {
    return s_lineState == LineState::Starting || s_lineState == LineState::Switching ||
           s_lineState == LineState::Releasing;
}

const char* lineStateName(LineState s) {
    switch (s) {
        case LineState::Off:       return "Off";
        case LineState::Starting:  return "Starting";
        case LineState::Active:    return "Active";
        case LineState::Switching: return "Switching";
        case LineState::Releasing: return "Releasing";
        case LineState::Failed:    return "Failed";
    }
    return "?";
}

void lineSetState(LineState st) {
    if (st == s_lineState) {
        return;
    }
    APP_LOG("line: %s -> %s\n", lineStateName(s_lineState), lineStateName(st));
    s_lineState = st;
    syncLineMute();  // мʼют проходу залежить від стану
}

// Є що звільняти: ресурси існують або модуль ще не в Off.
bool lineResourcesPresent() {
    return LinePath::resourcesUp() || LinePath::state() != LinePathState::Off;
}

// Пресет EQ входу idx (Settings::profiles) -> екземпляр EQ проходу.
void lineApplyEq(uint8_t idx) {
    if (idx == 0 || idx >= defaults::kInputCount || !LinePath::isReady()) {
        return;
    }
    const Settings st = SettingsStore::snapshot();
    LinePath::setEqAllDb(st.profiles[idx].eqGainsDb);
}

// Гучність + EQ поточного зовнішнього входу -> прохід.
void lineApplyParams() {
    lineApplyVolume();
    lineApplyEq(s_input);
}

// Реле входу + гучність + EQ. Викликати лише коли прохід цифрово замʼючений або не працює.
void lineApplyChipAndParams() {
    applyRelayForInput();
    lineApplyParams();
    s_lineApplied = true;
}

void lineBeginRelease() {
    s_lineRelStep = 0;
    s_lineRelCalled = false;
    s_lineT0 = millis();
    lineSetState(LineState::Releasing);
}

// Свіжий старт проходу (стан Off / Failed). Чип і параметри надсилаються перед requestStart().
void lineBegin() {
    s_lineAfter = LineAfter::Auto;
    s_lineRetryUsed = false;
    s_lineApplied = false;
    s_lineStep = LineStartStep::WaitIdle;
    s_lineT0 = millis();
    lineSetState(LineState::Starting);
}

// Вхід<->вхід без перезапуску проходу (повторний виклик під час Switching перезапускає цикл).
void lineSwitch() {
    s_lineSwitchStep = 0;
    s_lineT0 = millis();
    lineSetState(LineState::Switching);
}

// Невдача старту / втрата проходу: звільнити ресурси, один автоповтор, далі Failed.
void lineFail(const char* why) {
    APP_LOG("line: FAILED (%s) err=%s state=%s\n", why, LinePath::errorName(LinePath::lastError()),
            LinePath::stateName(LinePath::state()));
    s_lineAfter = s_lineRetryUsed ? LineAfter::Fail : LineAfter::Retry;
    s_lineRetryUsed = true;
    lineBeginRelease();
}

// Зовнішній запит звільнення (standby / перезапуск / OTA): без радіо й без автоповторів.
void lineRelease() {
    s_lineAfter = LineAfter::Auto;
    s_lineRetryUsed = false;
    switch (s_lineState) {
        case LineState::Starting:
        case LineState::Active:
        case LineState::Switching:
            lineBeginRelease();
            break;
        case LineState::Failed:
            lineSetState(LineState::Off);
            break;
        default:
            break;  // Off / Releasing
    }
}

// Ресурси звільнено (або вийшов час): вирішуємо, що далі, за ПОТОЧНИМ станом пристрою.
void lineReleaseDone(uint32_t now) {
    const LineAfter after = s_lineAfter;
    s_lineAfter = LineAfter::Auto;
    lineSetState(LineState::Off);
    if (logicallyStandby() || s_mode == Mode::OtaUpdate || restarting()) {
        return;  // тиша; новий старт — при виході зі standby / невдачі OTA
    }
    if (s_input != 0) {
        if (after == LineAfter::Fail) {
            lineSetState(LineState::Failed);
            APP_LOG("line: giving up, silent until the input is selected again\n");
            return;
        }
        s_lineApplied = false;
        if (after == LineAfter::Retry) {
            s_lineStep = LineStartStep::WaitRetry;
            s_lineT0 = now + cfg::kLineRetryDelayMs;
            APP_LOG("line: retry in %u ms\n", static_cast<unsigned>(cfg::kLineRetryDelayMs));
        } else {
            s_lineStep = LineStartStep::WaitIdle;
            s_lineT0 = now;
        }
        lineSetState(LineState::Starting);
        return;
    }
    // Вхід — радіо: RAM проходу вже звільнена, стартуємо потік. Settle перезапускаємо: радіо до розмʼюту в мʼюті.
    startTransition();
    radioBegin();
    publishStationName();
}

void lineServiceStarting(uint32_t now) {
    if (s_lineStep == LineStartStep::WaitRetry) {
        if (reached(now, s_lineT0)) {
            s_lineStep = LineStartStep::WaitIdle;
            s_lineT0 = now;
            s_lineApplied = false;
        }
        return;
    }
    if (s_lineStep == LineStartStep::WaitIdle) {
        const bool ready = LinePath::isReady();
        if (ready && AudioPlayer::state() == PlayerState::Idle) {
            if (!s_lineApplied) {
                lineApplyChipAndParams();  // прохід ще не працює: міняти чип безпечно
            }
            if (LinePath::requestStart()) {
                s_lineStep = LineStartStep::WaitRunning;
                s_lineT0 = now;
                APP_LOG("line: start requested (input %u)\n", static_cast<unsigned>(s_input));
                return;
            }
            if (LinePath::lastError() == LinePathError::AllocFailed) {
                lineFail("alloc failed");
                return;
            }
        }
        if (static_cast<uint32_t>(now - s_lineT0) >= cfg::kLineIdleWaitMs) {
            lineFail(ready ? "player not idle or start refused" : "line path not ready");
        }
        return;
    }
    // WaitRunning
    const uint32_t elapsed = now - s_lineT0;
    const LinePathState st = LinePath::state();
    if (st == LinePathState::Running && reached(now, s_lineRelayMs + input_selector_cfg::kRelaySettleMs)) {
        s_lineRetryUsed = false;  // успішний старт повертає право на автоповтор
        lineSetState(LineState::Active);  // syncLineMute() розмʼютить, якщо Settle / мʼют користувача не заважають
        return;
    }
    if (st == LinePathState::Off && LinePath::lastError() != LinePathError::None &&
        elapsed >= cfg::kLineErrCheckDelayMs) {
        lineFail("start aborted");
        return;
    }
    if (elapsed >= cfg::kLineStartTimeoutMs) {
        lineFail("start timeout");
    }
}

void lineServiceSwitching(uint32_t now) {
    if (!LinePath::resourcesUp() || LinePath::state() == LinePathState::Off) {
        lineFail("lost while switching");
        return;
    }
    if (s_lineSwitchStep == 0) {
        if (reached(now, s_lineT0 + cfg::kExtMuteSettleMs)) {
            lineApplyChipAndParams();  // прохід цифрово замʼючений (state != Active)
            s_lineSwitchStep = 1;
        }
        return;
    }
    if (reached(now, s_lineRelayMs + input_selector_cfg::kRelaySettleMs)) {
        lineSetState(LineState::Active);
    }
}

void lineServiceReleasing(uint32_t now) {
    if (s_lineRelStep == 0) {
        const bool present = lineResourcesPresent();
        if (present && static_cast<uint32_t>(now - s_lineT0) < cfg::kExtMuteSettleMs) {
            return;  // чекаємо, поки цифровий мʼют дозгасне
        }
        s_lineRelStep = 1;
        s_lineT0 = now;
        if (present) {
            s_lineRelCalled = LinePath::returnToRadio();
            APP_LOG("line: returnToRadio %s\n", s_lineRelCalled ? "requested" : "refused (will retry)");
        }
    }
    if (!lineResourcesPresent()) {
        lineReleaseDone(now);
        return;
    }
    if (!s_lineRelCalled) {
        s_lineRelCalled = LinePath::returnToRadio();
    }
    if (static_cast<uint32_t>(now - s_lineT0) >= cfg::kLineReleaseTimeoutMs) {
        APP_LOG("line: release timeout (resources=%d state=%s), continuing\n",
                LinePath::resourcesUp() ? 1 : 0, LinePath::stateName(LinePath::state()));
        lineReleaseDone(now);
    }
}

// Щотакту з tickLocked().
void lineService(uint32_t now) {
    switch (s_lineState) {
        case LineState::Starting:
            lineServiceStarting(now);
            break;
        case LineState::Active:
            if (!LinePath::resourcesUp() || LinePath::state() == LinePathState::Off) {
                lineFail("lost while active");  // напр. хтось викликав lp.radio повз контролер
            }
            break;
        case LineState::Switching:
            lineServiceSwitching(now);
            break;
        case LineState::Releasing:
            lineServiceReleasing(now);
            break;
        default:
            break;  // Off / Failed
    }
}

// Спрямувати звук на s_input (s_input / профіль уже встановлено; пристрій «увімкнений»).
void routeInput() {
    if (s_input == 0) {
        switch (s_lineState) {
            case LineState::Off:
            case LineState::Failed:
                lineSetState(LineState::Off);
                radioBegin();
                break;
            case LineState::Releasing:
                s_lineAfter = LineAfter::Auto;  // радіо стартує, коли ресурси звільнено
                break;
            default:
                s_lineAfter = LineAfter::Auto;
                lineBeginRelease();  // мʼют -> returnToRadio() -> радіо
                break;
        }
        return;
    }
    switch (s_lineState) {
        case LineState::Off:
        case LineState::Failed:
            lineBegin();
            break;
        case LineState::Releasing:
            s_lineAfter = LineAfter::Auto;  // після звільнення — свіжий старт на новому вході
            s_lineRetryUsed = false;
            break;
        case LineState::Starting:
            if (s_lineStep == LineStartStep::WaitRunning) {
                lineApplyChipAndParams();  // ресурси створюються, прохід ще замʼючений
            } else {
                s_lineApplied = false;  // перенадіслати з новим входом перед requestStart()
            }
            break;
        case LineState::Active:
        case LineState::Switching:
            lineSwitch();
            break;
    }
}

// Вхід<->вхід не потребує Phase::Settle (аналогового чипа/підсилювача на цьому шляху немає).
inline bool lineSkipsSettle(uint8_t prev, uint8_t next) {
    return prev != 0 && next != 0;
}
#else  // LINE_PATH_ENABLE == 0: зовнішні входи мовчать, радіо як у P47a
inline bool lineBusy() { return false; }
inline void lineRelease() {}
inline void lineService(uint32_t) {}
inline bool lineSkipsSettle(uint8_t, uint8_t) { return false; }
inline void routeInput() {
    if (s_input == 0) {
        radioBegin();
    } else {
        applyRelayForInput();  // реле-селектор (звуку немає: прохід вимкнено збіркою)
    }
}
#endif

// Увімкнення: режим за s_input, перехід, вхід у чіпі, запуск потоку для Radio.
// Вхід/станцію викликач уже поклав у s_input/s_station.
void powerOnTransition() {
    s_mode = (s_input == 0) ? Mode::Radio : Mode::ExternalInput;
    s_target = AdjustTarget::Volume;  // [Prompt 31] після вмикання/виходу зі standby — гучність
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    startTransition();
    ampRequestOn();  // [Prompt 23c]: після мʼюту; прогрів іде паралельно з налаштуванням чипа
    // [Prompt 21b/46a] Усе ще під мʼютом: профіль входу -> робоча гучність, вхід у чіп, потім нейтральні
    // гучність/тембр/баланс/gain/loudness. Цифрову гучність піднімає DSP після розмʼюту в tick().
    loadActiveFromProfile(s_input);
    routeInput();  // [Prompt 47c] радіо: чип + EQ + потік; зовнішній вхід: старт проходу (чип — перед requestStart())
    publishStationName();  // [Prompt 20b]
}

// --- Дії -------------------------------------------------------------------
void enterStandby() {
    APP_LOG("standby on\n");
    s_phase = Phase::None;
    s_target = AdjustTarget::Volume;  // [Prompt 31]
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    s_mode = Mode::Standby;
    applyHardwareMute(true);  // Standby входить у desiredMute(): [P47a] радіо згасає ДО stopStream()
    ampRequestOff();      // [Prompt 23c]: пін LOW через kAmpOffDelayMs (у tick())
    stopStream();
    lineRelease();        // [Prompt 47c]: прохід замʼючено (desiredMute()), returnToRadio() через kExtMuteSettleMs
    persist();
    SettingsStore::flush();
    s_persistNeeded = false;
}

// [Prompt 28] ЄДИНЕ місце рішення «чи доступний вхід»: індекс у межах логічних входів чипа ([Prompt 46b]:
// TDA7318 = 5, PT2313L = 4 = радіо + входи чіпа) і,
// в офлайні, не радіо (вхід 0). Усі вибори входу (кнопки, пульт, веб, вихід зі standby) йдуть
// через цю функцію.
// [Prompt 39] + дозволено маскою входів (вхід 0 завжди дозволений: біт 0 нормалізується).
bool inputEnabled(uint8_t idx) {
    return idx < defaults::kInputCount && ((s_inputMask >> idx) & 1u) != 0;
}

bool inputAvailable(uint8_t idx) {  // [P48a] = маска входів (+ офлайн: без радіо)
    return inputEnabled(idx) && !(s_offline && idx == 0);
}

// Текстова причина недоступності входу для логів (лише коли inputAvailable(idx) == false).
const char* inputUnavailableWhy(uint8_t idx) {
    if (idx >= defaults::kInputCount) return "no such input";
    if (!inputEnabled(idx)) return "disabled";
    return "offline";
}

// [Prompt 39] Чи є хоч один дозволений зовнішній вхід (1..inputCount-1) — потрібен офлайн-режиму,
// де радіо недоступне. Офлайн-стан сюди не входить (зовнішні входи від нього не залежать).
bool anyExternalInputEnabled() {
    for (uint8_t i = 1; i < defaults::kInputCount; ++i) {
        if (inputEnabled(i)) {
            return true;
        }
    }
    return false;
}

// Перший доступний вхід за порядком 0..inputCount-1 (в офлайні це перший не-радіо вхід).
uint8_t firstAvailableInput() {
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        if (inputAvailable(i)) {
            return i;
        }
    }
    // [Prompt 39] Недосяжно: радіо (0) дозволене завжди, а офлайн-режим не стартує без дозволеного
    // зовнішнього входу (startOfflineLocked()).
    return 0;
}

void leaveStandby() {
    captureActiveProfile();  // [Prompt 21b] профіль «старого» входу, поки s_input ще його
    const Settings st = SettingsStore::snapshot();
    s_input = (st.lastInput < defaults::kInputCount) ? st.lastInput : 0;
    if (!inputAvailable(s_input)) {  // [Prompt 28] офлайн: lastInput = радіо -> перший не-радіо вхід
        s_input = firstAvailableInput();
    }
    s_station = (st.lastStation < stationCount()) ? st.lastStation : 0;
    APP_LOG("standby off: input=%u station=%u\n", static_cast<unsigned>(s_input),
            static_cast<unsigned>(s_station));
    powerOnTransition();
}

void togglePower() {
    if (s_mode == Mode::Standby) {
        leaveStandby();
    } else {
        enterStandby();
    }
}

void toggleUserMute() {
    s_userMute = !s_userMute;
    applyHardwareMute();  // [P47a] цифровий мʼют/розмʼют радіо: плавно (20 / 100 мс)
    s_persistNeeded = true;
    APP_LOG("mute %s\n", s_userMute ? "on" : "off");
}

void changeInput(uint8_t newIdx) {
    if (newIdx == s_input) {
        return;
    }
    if (!inputAvailable(newIdx)) {  // [Prompt 28] сторожа: у офлайні радіо недоступне з будь-якого шляху
        APP_LOG("input %u unavailable (%s)\n", static_cast<unsigned>(newIdx),
                inputUnavailableWhy(newIdx));  // [Prompt 39]: + disabled
        return;
    }
    APP_LOG("input %u -> %u (%s)\n", static_cast<unsigned>(s_input),
            static_cast<unsigned>(newIdx),
            newIdx < defaults::kInputCount ? defaults::kInputNames[newIdx] : "?");

    captureActiveProfile();  // [Prompt 21b] профіль старого входу фіксуємо ДО зміни s_input
    const uint8_t prevIdx = s_input;
    s_input = newIdx;
    s_mode = (newIdx == 0) ? Mode::Radio : Mode::ExternalInput;
    // [Prompt 47c] Вхід<->вхід через прохід: без Phase::Settle (мʼют проходу веде автомат Switching).
    if (!lineSkipsSettle(prevIdx, newIdx)) {
        startTransition();  // мʼют (цифровий радіо + прохід + чип)
    }
    if (prevIdx == 0) {
        stopStream();  // залишаємо радіо
    }
    // [Prompt 21b/46a] Профіль нового входу -> робоча гучність (s_volume = ціль майбутнього розмʼюту).
    loadActiveFromProfile(newIdx);
    // [Prompt 47c] Далі: радіо — чип, EQ входу 0, потік (після звільнення проходу, якщо він був); зовнішній —
    // старт проходу / Switching (чип, гучність і EQ входу надсилаються, коли прохід цифрово замʼючений).
    routeInput();
    publishStationName();  // [Prompt 20b]: вхід Radio -> назва зі списку, інакше порожньо
    s_persistNeeded = true;
}

void stepInput(int dir) {
    const int count = defaults::kInputCount;  // [P48a] 3 логічні входи; вимкнені маскою пропускаються
    // [Prompt 28] Недоступні входи (в офлайні — радіо) пропускаються. Без офлайну перший же
    // крок доступний, тож поведінка не змінилась.
    int n = s_input;
    for (int i = 0; i < count; ++i) {
        n = (n + dir + count) % count;
        if (inputAvailable(static_cast<uint8_t>(n))) {
            changeInput(static_cast<uint8_t>(n));
            return;
        }
    }
}

void selectInput(uint8_t idx) {
    if (!inputAvailable(idx)) {  // [Prompt 28] у межах чипа й, в офлайні, не радіо
        APP_LOG("input %u unavailable (%s) - ignored\n", static_cast<unsigned>(idx),
                inputUnavailableWhy(idx));  // [Prompt 39]: no such input / disabled / offline
        return;
    }
    changeInput(idx);
}

// [Prompt 28] Запуск офлайн-режиму. Лише з Mode::WifiSetup; викликати під s_lock. Порядок:
// 1) s_offline (радіо одразу стає недоступним), 2) вимкнення Wi-Fi (неблокуюче, виконує задача
// WifiManager), 3) перший не-радіо вхід через changeInput() — мʼют, профіль входу, розмʼют
// як при звичайній зміні входу (підсилювач у WifiSetup уже працює: прогріву немає).
bool startOfflineLocked() {
    if (s_offline || restarting() || s_mode != Mode::WifiSetup) {
        return false;
    }
    // [Prompt 39] Офлайн без радіо потребує хоч одного дозволеного зовнішнього входу, інакше звуку
    // не буде взагалі, а екран лишився б у WifiSetup до перезапуску.
    if (!anyExternalInputEnabled()) {
        APP_LOG("offline refused: no external input enabled\n");
        return false;
    }
    s_offline = true;
    s_playPending = false;
    APP_LOG("offline: requested in WifiSetup, Wi-Fi off\n");
    WifiManager::stop();
    // Уже на доступному не-радіо вході — лишаємось на ньому; інакше перший не-радіо вхід.
    const uint8_t first = inputAvailable(s_input) ? s_input : firstAvailableInput();
    APP_LOG("offline: start on input %u (%s)\n", static_cast<unsigned>(first),
            first < defaults::kInputCount ? defaults::kInputNames[first] : "?");
    if (first != s_input) {
        changeInput(first);
    } else {
        // Той самий не-радіо вхід: changeInput() нічого б не зробив, тож виходимо з WifiSetup
        // вручну (мʼют -> розмʼют, профіль входу вже застосований).
        s_mode = Mode::ExternalInput;
        startTransition();
    }
    return true;
}

void changeStation(uint16_t idx) {
    char nm[sizeof(Station::name)];
    stationLabel(idx, nm, sizeof(nm));
    APP_LOG("station %u -> %u (%s)\n", static_cast<unsigned>(s_station),
            static_cast<unsigned>(idx), nm);
    s_station = idx;
    startTransition();  // [P47a] радіо згасає до playUrl() нової станції
    startStream();
    publishStationName();  // [Prompt 20b]
    s_persistNeeded = true;
}

void stepStation(int dir) {
    const int count = stationCount();
    if (count < 1) {
        return;
    }
    if (count == 1) {
        // [Prompt 14] Єдина станція: якщо індекс застарів (список скоротився) —
        // переходимо на неї; якщо вже на ній — нічого не робимо.
        if (s_station != 0) {
            changeStation(0);
        }
        return;
    }
    clampStationIndex();  // [Prompt 14] крок рахуємо від дійсного індексу
    changeStation(static_cast<uint16_t>((static_cast<int>(s_station) + dir + count) % count));
}

void togglePlayPause() {
    // Idle = пауза/зупинка; будь-який інший стан (Connecting/Buffering/Playing/
    // Error/Reconnecting) вважаємо «грає» — OK зупиняє, зокрема скасовує backoff.
    // [P47a] Пауза: плавний цифровий мʼют радіо -> stop() (pauseRadio()); XSMT ЦАП керує сам плеєр.
    if (AudioPlayer::state() != PlayerState::Idle || s_playPending) {
        pauseRadio();
        APP_LOG("pause\n");
    } else {
        char nm[sizeof(Station::name)];
        stationLabel(s_station, nm, sizeof(nm));
        APP_LOG("play %s\n", nm);
        startStream();
    }
}

// Один «крок» цілі t у напрямку знака d (|d| = кількість detent-ів).
// [Prompt 46a] Регулюється лише гучність (бас/дискант/баланс/gain і цикл цілей прибрано).
void adjustParam(AdjustTarget t, int d) {
    if (t != AdjustTarget::Volume) {
        return;
    }
    if (setVolumeTarget(static_cast<int>(s_volume) + d * cfg::kVolumeStep)) {
        s_persistNeeded = true;
        APP_LOG("volume=%d\n", static_cast<int>(s_volume));
    }
}

// --- Список станцій --------------------------------------------------------
void openStationList() {
    if (s_mode != Mode::Radio) {
        return;
    }
    if (stationCount() == 0) {
        APP_LOG("list not opened: no stations\n");
        return;
    }
    clampStationIndex();  // [Prompt 14]
    s_menuCtx = MenuContext::StationList;
    s_menuSel = s_station;
    s_mode = Mode::Menu;
    APP_LOG("list open sel=%u\n", static_cast<unsigned>(s_menuSel));
}

void closeStationList() {
    s_menuCtx = MenuContext::None;
    s_mode = Mode::Radio;  // список відкривається лише з Radio
    APP_LOG("list close\n");
}

void moveListSelection(int d) {
    const int count = stationCount();
    if (count <= 0) {
        return;  // список спорожнів (майбутній веб-імпорт): ділити на 0 не можна
    }
    if (s_menuSel >= count) {
        s_menuSel = static_cast<uint16_t>(count - 1);  // [Prompt 14] список скоротився
    }
    int n = (static_cast<int>(s_menuSel) + d) % count;
    if (n < 0) n += count;
    s_menuSel = static_cast<uint16_t>(n);
    char nm[sizeof(Station::name)];
    stationLabel(s_menuSel, nm, sizeof(nm));
    APP_LOG("list sel=%u (%s)\n", static_cast<unsigned>(s_menuSel), nm);
}

void selectFromList() {
    const uint16_t sel = s_menuSel;
    closeStationList();
    if (sel >= stationCount()) {
        return;  // список скоротився, поки був відкритий
    }
    // Та сама станція вже грає — нічого не робимо; якщо стоїть пауза — запускаємо.
    if (sel != s_station || AudioPlayer::state() == PlayerState::Idle) {
        changeStation(sel);
    }
}

// --- Обробники подій -------------------------------------------------------
void handleMenuEvent(const Event& e) {
    const int step = (e.delta > 0) ? e.delta : 1;
    switch (e.action) {
        case Action::UP:      moveListSelection(-1); break;
        case Action::DOWN:    moveListSelection(+1); break;
        case Action::ENC_CCW: moveListSelection(-step); break;
        case Action::ENC_CW:  moveListSelection(+step); break;
        case Action::OK:
            if (!e.longPress && !e.repeat) selectFromList();
            break;
        case Action::BACK:
        case Action::MENU:
        case Action::LEFT:
            if (!e.longPress && !e.repeat) closeStationList();
            break;
        default:
            break;
    }
}

void handleMainEvent(const Event& e) {
    const int step = (e.delta > 0) ? e.delta : 1;
    const bool plain = !e.repeat && !e.longPress;
    switch (e.action) {
        case Action::UP:
            if (plain) stepInput(cfg::kInputUpStep);
            break;
        case Action::DOWN:
            if (plain) stepInput(cfg::kInputDownStep);
            break;
        case Action::LEFT:
            if (plain && s_input == 0 && s_mode != Mode::WifiSetup) {
                stepStation(cfg::kStationLeftStep);
            }
            break;
        case Action::RIGHT:
            if (plain && s_input == 0 && s_mode != Mode::WifiSetup) {
                stepStation(cfg::kStationRightStep);
            }
            break;
        case Action::OK:
            if (s_input == 0 && s_mode != Mode::WifiSetup) {  // [Prompt 12]: у WifiSetup потоку немає
                if (e.longPress) {
                    openStationList();
                } else if (!e.repeat) {
                    togglePlayPause();
                }
            }
            break;
        case Action::ENC_CW:  adjustParam(s_target, +step); break;
        case Action::ENC_CCW: adjustParam(s_target, -step); break;
        case Action::INPUT_RADIO: if (plain) selectInput(0); break;
        case Action::INPUT_TV:    if (plain) selectInput(1); break;
        case Action::INPUT_PC:    if (plain) selectInput(2); break;
        case Action::INPUT_AUX:   // [P48a] входу AUX немає: дія лишається в enum (мапа IR), без дії
            if (plain) APP_LOG("INPUT_AUX ignored (no such input)\n");
            break;
        default:
            APP_LOG("event ignored (not in Prompt 8 scope)\n");
            break;
    }
}

// --- [Prompt 15] Навчання IR -------------------------------------------------
// Явний мапінг (як PlayerState -> StreamStatus): AppState не залежить від ir_rc5.h.
IrLearnStatus mapIrStatus(IrRc5::LearnStatus st) {
    switch (st) {
        case IrRc5::LearnStatus::Idle:     return IrLearnStatus::Idle;
        case IrRc5::LearnStatus::Waiting:  return IrLearnStatus::Waiting;
        case IrRc5::LearnStatus::Confirm:  return IrLearnStatus::Confirm;
        case IrRc5::LearnStatus::Success:  return IrLearnStatus::Success;
        case IrRc5::LearnStatus::Timeout:  return IrLearnStatus::Timeout;
        case IrRc5::LearnStatus::Conflict: return IrLearnStatus::Conflict;
    }
    return IrLearnStatus::Idle;
}

// Навчання триває й чекає користувача (на відміну від Success/Timeout/Idle).
bool irStatusActive(IrLearnStatus st) {
    return st == IrLearnStatus::Waiting || st == IrLearnStatus::Confirm ||
           st == IrLearnStatus::Conflict;
}

bool irStatusFinal(IrLearnStatus st) {
    return st == IrLearnStatus::Success || st == IrLearnStatus::Timeout;
}

struct IrPubCtx {
    IrLearnStatus status;
    Action target;
    Action other;
};

void applyIrPub(AppStateData& s, void* c) {
    const IrPubCtx* p = static_cast<const IrPubCtx*>(c);
    s.irLearnStatus = p->status;
    s.irLearnTarget = p->target;
    s.irLearnConflictWith = p->other;
}

void publishIrLearn(IrLearnStatus st, Action target, Action other) {
    if (st == s_irPubStatus && target == s_irPubTarget && other == s_irPubOther) {
        return;
    }
    s_irPubStatus = st;
    s_irPubTarget = target;
    s_irPubOther = other;
    IrPubCtx p = {st, target, other};
    AppState::modify(applyIrPub, &p);
}

// Вихід з Mode::IrLearn у збережений режим. Викликати лише під s_lock і лише коли
// s_mode == IrLearn. IrRc5 приводимо в Idle (cancelLearn скидає й Success/Timeout) —
// це гарантує, що звичайна генерація Action з пульта відновлена. Статус у AppState
// НЕ чіпаємо: викликач сам вирішує, лишати «липкий» результат чи скидати в Idle.
void leaveIrLearnMode() {
    IrRc5::cancelLearn();
    s_mode = s_irPrevMode;
    s_irHolding = false;
    APP_LOG("IR learn end, mode -> %s\n", modeName(s_mode));
    publishState();
}

// Явне скасування: одразу назад, статус Idle.
void abortIrLearn() {
    publishIrLearn(IrLearnStatus::Idle, s_irPubTarget, s_irPubTarget);
    leaveIrLearnMode();
}

// Щотакту (tickLocked): переносить стан IrRc5 в AppState і завершує навчання.
void syncIrLearn(uint32_t now) {
    if (s_mode != Mode::IrLearn) {
        return;
    }
    const IrLearnStatus st = mapIrStatus(IrRc5::status());
    if (st == IrLearnStatus::Idle) {
        // Навчання зникло з-під нас (хтось викликав IrRc5::cancelLearn() повз контролер).
        abortIrLearn();
        return;
    }
    const Action target = IrRc5::learnTarget();
    Action other = target;
    if (st == IrLearnStatus::Conflict) {
        Action o;
        if (IrRc5::learnConflictWith(o)) {
            other = o;
        }
    }
    publishIrLearn(st, target, other);

    if (irStatusFinal(st)) {
        if (!s_irHolding) {
            s_irHolding = true;
            s_irHoldUntilMs = now + ir_learn_ui_cfg::kResultHoldMs;
            APP_LOG("IR learn result %s, hold %u ms\n",
                    st == IrLearnStatus::Success ? "Success" : "Timeout",
                    static_cast<unsigned>(ir_learn_ui_cfg::kResultHoldMs));
        } else if (reached(now, s_irHoldUntilMs)) {
            leaveIrLearnMode();  // статус Success/Timeout лишається в AppState
        }
    } else {
        s_irHolding = false;
    }
}

// Події під час навчання. Звичайні кнопки/енкодер ігноруються; пульт і так
// призупинений в IrRc5. POWER -> Standby, MENU/BACK -> попередній режим.
void handleIrLearnEvent(const Event& e) {
    if (e.repeat || e.longPress) {
        return;
    }
    switch (e.action) {
        case Action::POWER: {
            const bool wasStandby = (s_irPrevMode == Mode::Standby);
            abortIrLearn();
            if (!wasStandby) {
                enterStandby();
            }
            break;
        }
        case Action::MENU:
        case Action::BACK:
            abortIrLearn();
            break;
        default:
            break;
    }
}

bool beginIrLearnLocked(Action target) {
    if (s_mode == Mode::OtaUpdate || restarting()) {
        return false;  // [Prompt 16]; [Prompt 28] і під час перезапуску
    }
    if (s_mode == Mode::IrLearn) {
        if (irStatusActive(mapIrStatus(IrRc5::status()))) {
            return false;  // навчання триває: спершу cancelIrLearn()
        }
        leaveIrLearnMode();  // на екрані ще результат попереднього - знімаємо одразу
    }
    if (!IrRc5::beginLearn(target)) {
        return false;
    }
    s_irPrevMode = s_mode;
    s_mode = Mode::IrLearn;
    s_irHolding = false;
    IrLearnStatus st = mapIrStatus(IrRc5::status());
    if (st == IrLearnStatus::Idle) {
        st = IrLearnStatus::Waiting;
    }
    publishIrLearn(st, target, target);
    APP_LOG("IR learn begin, mode %s -> IrLearn\n", modeName(s_irPrevMode));
    publishState();
    return true;
}

bool confirmIrOverwriteLocked() {
    if (s_mode != Mode::IrLearn || mapIrStatus(IrRc5::status()) != IrLearnStatus::Conflict) {
        return false;
    }
    if (!IrRc5::confirmOverwrite()) {
        return false;
    }
    syncIrLearn(millis());  // одразу публікуємо Success (запускає паузу показу результату)
    return true;
}

void cancelIrLearnLocked() {
    if (s_mode == Mode::IrLearn) {
        abortIrLearn();
        return;
    }
    IrRc5::cancelLearn();  // поза навчанням: лише скидаємо «липкий» результат
    publishIrLearn(IrLearnStatus::Idle, s_irPubTarget, s_irPubTarget);
}

// --- [Prompt 16] OTA ---------------------------------------------------------
void applyOtaPub(AppStateData& s, void* c) {
    s.otaProgress = *static_cast<uint8_t*>(c);
}

void publishOtaProgress(uint8_t p) {
    s_otaPubPercent = p;
    AppState::modify(applyOtaPub, &p);
}

// Викликати лише під s_lock. Повертає збережений Mode і відновлює звук. Standby: мʼют
// лишається (desiredMute()), потік не потрібен. Інакше — мʼют, розмʼют у tick() через Settle.
void leaveOtaLocked() {
    if (s_mode != Mode::OtaUpdate) {
        return;
    }
    s_mode = s_otaPrevMode;
    APP_LOG("OTA end, mode -> %s\n", modeName(s_mode));
    if (s_mode == Mode::Standby) {
        applyHardwareMute();
    } else {
        startTransition();
        if (s_input != 0) {
            routeInput();  // [Prompt 47c] прохід звільнено при beginOta(): запускаємо знову
        } else if (s_mode == Mode::Radio || s_mode == Mode::Menu) {
            startStream();  // сам відкладе, якщо немає Wi-Fi
        }
    }
    publishState();
    publishOtaProgress(0);
}

// --- [Prompt 28] «Тихий» перезапуск -------------------------------------------
// Викликати лише під s_lock. Послідовність (без блокувань, окрім ≤ kRadioMuteWaitMs згасання радіо
// [P47a]): мʼют (desiredMute() уже true через restarting()) -> стоп потоку (плеєр сам мʼютить XSMT)
// -> ampRequestOff() (пін LOW через kAmpOffDelayMs, P23c) -> restartService() чекає LOW, паузу
// kRestartMuteMs і ESP.restart().
void beginRestart() {
    if (restarting()) {
        return;
    }
    APP_LOG("restart: POWER held, quiet restart\n");
    s_phase = Phase::None;  // Settle скасовано (як у beginOta)
    s_playPending = false;
    s_restartPhase = RestartPhase::WaitAmp;
    s_restartDeadlineMs = millis() + cfg::kRestartMaxWaitMs;
    applyHardwareMute(true);  // мʼют ПЕРШИМ ([P47a]: радіо згасає до stopStream())
    if (AudioPlayer::state() != PlayerState::Idle) {
        stopStream();  // плеєр мʼютить XSMT ЦАП
    } else {
        s_playUrlValid = false;
    }
    ampRequestOff();  // ПІСЛЯ applyHardwareMute(), як у enterStandby()
    lineRelease();    // [Prompt 47c] прохід замʼючено (restarting() у desiredMute()), звільняємо ресурси
    // Остання зміна гучності/входу могла не дійти до NVS (запис із дебаунсом) — скидаємо зараз.
    persist();
    SettingsStore::flush();
    s_persistNeeded = false;
    publishState();  // AppState.restarting -> екран «Restarting...»
}

// Викликається щотакту з tickLocked(). ESP.restart() не повертається.
void restartService(uint32_t now) {
    if (s_restartPhase == RestartPhase::None) {
        return;
    }
    if (s_restartPhase == RestartPhase::WaitAmp) {
        const bool deadline = reached(now, s_restartDeadlineMs);
        if (!ampBusy() || deadline) {  // пін LOW (або підсилювача немає) чи вичерпано страховку
            s_restartPhase = RestartPhase::Pause;
            s_restartAtMs = now + cfg::kRestartMuteMs;
            APP_LOG("restart: amp %s, pause %u ms\n", deadline && ampBusy() ? "NOT low (timeout)" : "low",
                    static_cast<unsigned>(cfg::kRestartMuteMs));
        }
        return;
    }
    if (reached(now, s_restartAtMs)) {
        APP_LOG("restart: now\n");
        Serial.flush();
        ESP.restart();
    }
}

// [Prompt 32] Спливне вікно параметра. notifyPopup() лише фіксує, ЩО показати, і збільшує лічильник;
// публікація — publishState() наприкінці handleLocked(). Непідтримуваний чипом параметр не
// показуємо (значення 0 вводило б в оману).
void notifyPopup(AdjustTarget t) {
    if (!targetSupported(t)) {
        return;
    }
    s_popupTarget = t;
    ++s_popupSeq;  // uint8_t: перехід 255 -> 0 штатний, UI порівнює лише «змінилось чи ні»
}

// Викликається ПІСЛЯ обробки події і лише на головних екранах (Radio / ExternalInput).
// Енкодер: обертання й коротка клавіша показують ПОТОЧНУ ціль ([Prompt 46a]: завжди гучність).
// Утримання енкодера (мʼют) вікно не викликає. Прямі дії кнопок/IR показують свій параметр.
void popupForEvent(const Event& e) {
    switch (e.action) {
        case Action::ENC_CW:
        case Action::ENC_CCW:
            notifyPopup(s_target);
            break;
        case Action::ENC_PRESS:
            if (!e.longPress && !e.repeat) notifyPopup(s_target);
            break;
        case Action::VOL_UP:
        case Action::VOL_DOWN:     notifyPopup(AdjustTarget::Volume);  break;
        default:
            break;
    }
}

void handleLocked(const Event& e) {
#if APP_CONTROLLER_LOG_EVENTS && APP_CONTROLLER_DEBUG
    Serial.printf("[APP] evt %s %s%s%s delta=%d\n", sourceName(e.source), actionName(e.action),
                  e.repeat ? " repeat" : "", e.longPress ? " long" : "",
                  static_cast<int>(e.delta));
#endif

    // [Prompt 28] Довге утримання POWER = «тихий» перезапуск у БУДЬ-ЯКОМУ режимі (Standby,
    // WifiSetup, офлайн, звичайна робота, IrLearn, OtaUpdate). Стоїть ПЕРЕД гілкою OTA.
    if (e.action == Action::POWER && e.veryLongPress) {
        beginRestart();
        return;
    }
    if (restarting()) {
        return;  // [Prompt 28] перезапуск іде: решту подій ігноруємо
    }

    if (s_mode == Mode::OtaUpdate) {
        return;  // [Prompt 16] під час прошивки ігноруємо все, включно з POWER
    }

    if (s_mode == Mode::IrLearn) {
        // [Prompt 15] Навчання IR: окрема гілка (POWER тут НЕ перемикає Standby, а
        // скасовує навчання й веде в Standby).
        handleIrLearnEvent(e);
    } else if (e.action == Action::POWER) {
        if (!e.longPress && !e.repeat) {
            togglePower();
        } else {
            APP_LOG("POWER long/repeat ignored\n");  // [Prompt 28]: veryLong обробляється вище
        }
    } else if (s_mode == Mode::WifiSetup) {
        // [Prompt 28] Екран налаштування Wi-Fi (AP піднята, будь-який вхід): реагуємо лише на
        // POWER (гілка вище) і коротке OK / клік енкодера = офлайн-режим; усе інше ігноруємо.
        if (!e.repeat && !e.longPress &&
            (e.action == Action::OK || e.action == Action::ENC_PRESS)) {
            startOfflineLocked();
        }
    } else if (s_mode == Mode::Standby) {
        // Standby: усе, крім POWER, ігнорується.
        // [Prompt 12] WifiSetup тут більше не ігнорується: перемикання входу й
        // гучність працюють (радіо без Wi-Fi недоступне, зовнішні входи — так).
    } else {
        bool handled = true;
        switch (e.action) {
            // Глобальні дії: однакові на головному екрані й у списку.
            case Action::MUTE:
                if (!e.repeat && !e.longPress) toggleUserMute();
                break;
            case Action::VOL_UP:       adjustParam(AdjustTarget::Volume, +1); break;
            case Action::VOL_DOWN:     adjustParam(AdjustTarget::Volume, -1); break;
            // [Prompt 46a] Дії тембру/балансу/gain (enum не змінено — значення зберігаються в мапі IR):
            // ігноруються з одним коротким логом на натискання (повтори утримання не логуються).
            case Action::BASS_UP:
            case Action::BASS_DOWN:
            case Action::TREBLE_UP:
            case Action::TREBLE_DOWN:
            case Action::BALANCE_UP:
            case Action::BALANCE_DOWN:
            case Action::GAIN_UP:
            case Action::GAIN_DOWN:
                if (!e.repeat) APP_LOG("tone/balance/gain action ignored (controls removed)\n");
                break;
            case Action::ENC_PRESS:
                // [Prompt 46a] Довге утримання = мʼют; короткий клік більше не міняє ціль (лише вікно гучності).
                if (e.longPress) {
                    toggleUserMute();
                }
                break;
            default:
                handled = false;
                break;
        }
        if (!handled) {
            if (s_mode == Mode::Menu) {
                handleMenuEvent(e);
            } else {
                handleMainEvent(e);
            }
        }
        // [Prompt 32] Спливне вікно: лише головні екрани (у Menu енкодер рухає список).
        if (s_mode == Mode::Radio || s_mode == Mode::ExternalInput) {
            popupForEvent(e);
        }
    }

    publishState();
    if (s_persistNeeded) {
        persist();
        s_persistNeeded = false;
    }
}

// --- Wi-Fi і Mode::WifiSetup ------------------------------------------------
// [Prompt 12] AP піднято + вхід Radio -> WifiSetup; AP зникла -> назад у Radio.
// Зовнішні входи, Standby, Menu/IrLearn не чіпаємо. Режим міняється тут, у tick(),
// тож публікуємо стан одразу (handleLocked() цього не зробить).
void followWifiMode() {
    if (s_offline) {
        return;  // [Prompt 28] офлайн: Wi-Fi вимкнено, WifiSetup/Radio більше не настають
    }
    const bool ap = WifiManager::isApMode();
    Mode want = s_mode;
    // [Prompt 28] AP піднята -> екран WifiSetup на БУДЬ-ЯКОМУ вході (раніше лише на радіо);
    // у ньому все заблоковане, крім OK/POWER, а звук замʼючено (desiredMute()).
    if (ap && (s_mode == Mode::Radio || s_mode == Mode::ExternalInput || s_mode == Mode::Menu)) {
        want = Mode::WifiSetup;
    } else if (!ap && s_mode == Mode::WifiSetup) {
        want = (s_input == 0) ? Mode::Radio : Mode::ExternalInput;
    }
    if (want == s_mode) {
        return;
    }
    s_mode = want;
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    applyHardwareMute();  // [Prompt 28] вхід/вихід з WifiSetup міняє desiredMute()
    // WifiSetup: відкладений старт потоку не потрібен (AP завершується перезапуском).
    // Назад у Radio: дозволити відкладений старт, коли зʼявиться Wi-Fi.
    s_playPending = (want == Mode::Radio) && !s_offline;
    APP_LOG("mode -> %s (Wi-Fi)\n", modeName(s_mode));
    publishState();
}

// --- Періодика (розмʼют, таймери, синхронізація) ---------------------------
void tickLocked() {
    const uint32_t now = millis();

    ampService(now);  // [Prompt 23c]
    restartService(now);  // [Prompt 28] (ESP.restart() не повертається)
    lineService(now);     // [Prompt 47c] автомат проходу зовнішніх входів

    if (s_otaAbortPending) {  // [Prompt 16] otaFailed() не отримав мʼютекс
        s_otaAbortPending = false;
        leaveOtaLocked();
    }

    // [Prompt 23c] Розмʼют лише коли підсилювач прогрітий (або не потрібен).
    // [Prompt 47a] Програмної рампи немає: applyHardwareMute() знімає цифровий мʼют радіо, DSP сам плавно
    // (kRampMs) піднімає звук з першого блоку; мʼют користувача/Standby/WifiSetup лишаються (desiredMute()).
    // [Prompt 47c] Settle не завершується, поки прохід стартує / перемикається / звільняється.
    if (s_phase == Phase::Settle && reached(now, s_phaseStartMs + cfg::kUnmuteDelayMs) &&
        ampReady() && !lineBusy()) {
        s_phase = Phase::None;
        applyHardwareMute();
        APP_LOG("unmute, vol=%d (%.2f dB)\n", static_cast<int>(s_volume),
                static_cast<double>(s_input == 0 ? audio_cfg::radioVolumeDb(s_volume) : extVolumeDb(s_volume)));
    }

    if (desiredMute() != s_hwMute) {  // [P48a] страховка: цифровий мʼют завжди відповідає desiredMute()
        applyHardwareMute();
    }

    followWifiMode();  // [Prompt 12] ДОДАНО
    syncIrLearn(now);  // [Prompt 15] ДОДАНО

    if (s_playPending && s_input == 0 && !s_offline && !restarting() &&  // [Prompt 28]
        (s_mode == Mode::Radio || s_mode == Mode::Menu) && wifiUp()) {
        APP_LOG("Wi-Fi up: starting deferred play\n");
        startStream();
    }

    if (static_cast<uint32_t>(now - s_lastSyncMs) >= cfg::kSyncPeriodMs) {
        s_lastSyncMs = now;
        // [Prompt 14] Список міг змінитись з вебу: тримаємо s_station у межах і
        // публікуємо виправлений індекс (AppState.stationIndex, Settings.lastStation).
        if (clampStationIndex()) {
            publishState();
            persist();
            s_persistNeeded = false;
        }
        syncStationLevel();  // [Prompt 25]
        syncPlayer();
    }
}

void tick() {
    if (s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return;
    }
    tickLocked();
    xSemaphoreGiveRecursive(s_lock);
}

void taskMain(void*) {
    for (;;) {
        // Читаємо без мʼютекса: це лише вибір тайм-ауту, хибне значення безпечне.
        // [Prompt 15]: під час навчання IR теж активний (короткий) період опитування.
        const bool active =
            (s_phase != Phase::None) || (s_mode == Mode::IrLearn) ||
            lineBusy() ||  // [Prompt 47c]: автомат проходу рухається з активним періодом
            ampBusy() ||  // [Prompt 23c]
            restarting();  // [Prompt 28]: чекаємо пін/паузу з активним періодом
        Event ev;
        const TickType_t timeout =
            pdMS_TO_TICKS(active ? cfg::kActivePollMs : cfg::kIdlePollMs);
        if (EventBus::poll(ev, timeout)) {
            AppController::handleEvent(ev);
        }
        tick();
    }
}

// --- [Prompt 18] Команди з вебу -----------------------------------------------
// Викликати лише під s_lock. Перевірка меж і гейти — ДО будь-якої зміни стану, тож
// відмова (NotAllowed/OutOfRange/Unsupported) нічого не чіпає (список станцій на
// пристрої теж лишається відкритим).

// Станційні/плеєрні команди мають сенс лише на вході Radio, і не в WifiSetup (потоку немає).
bool webRadioGate(const char*& why) {
    if (s_input != 0) {
        why = "not_radio_input";
        return false;
    }
    if (s_mode == Mode::WifiSetup) {
        why = "wifi_setup";
        return false;
    }
    return true;
}

// Той самий критерій «грає», що в togglePlayPause(): будь-який стан, окрім Idle, або
// відкладений старт.
bool playerActive() {
    return AudioPlayer::state() != PlayerState::Idle || s_playPending;
}

WebCmdResult execWebCommandLocked(const WebCommand& c, const char*& why) {
    why = nullptr;
    if (restarting()) {  // [Prompt 28] наявний код "busy" (503): нових reason не додаємо
        why = "busy";
        return WebCmdResult::Busy;
    }
    if (s_mode == Mode::OtaUpdate) {
        why = "ota_in_progress";
        return WebCmdResult::NotAllowed;
    }
    if (s_mode == Mode::IrLearn) {
        why = "ir_learn_active";
        return WebCmdResult::NotAllowed;
    }
    const bool powerCmd =
        (c.type == WebCmdType::StandbySet || c.type == WebCmdType::StandbyToggle);
    // [Prompt 39] Налаштування масок входів дозволено і в Standby (після холодного старту пристрій
    // саме там, а сторінка налаштувань має працювати): стан звуку воно не чіпає.
    if (s_mode == Mode::Standby && !powerCmd && c.type != WebCmdType::InputsEnabledSet) {
        why = "standby";  // як і події: у Standby все, крім POWER, ігнорується
        return WebCmdResult::NotAllowed;
    }

    switch (c.type) {
        case WebCmdType::VolumeSet:
            if (c.value < audio_cfg::kVolumeUiMin || c.value > audio_cfg::kVolumeUiMax) {
                why = "volume_out_of_range";
                return WebCmdResult::OutOfRange;
            }
            if (setVolumeTarget(static_cast<int>(c.value))) {
                s_persistNeeded = true;
                APP_LOG("volume=%d (WEB)\n", static_cast<int>(s_volume));
            }
            break;

        case WebCmdType::VolumeStep: {
            const int d = clampInt(static_cast<int>(c.value), -1000, 1000);
            if (setVolumeTarget(static_cast<int>(s_volume) + d)) {  // обрізається до меж
                s_persistNeeded = true;
                APP_LOG("volume=%d (WEB step %d)\n", static_cast<int>(s_volume), d);
            }
            break;
        }

        case WebCmdType::MuteSet:
            if (s_userMute != c.flag) {
                toggleUserMute();
            }
            break;

        case WebCmdType::MuteToggle:
            toggleUserMute();
            break;

        case WebCmdType::GainSet:
            // [Prompt 48a] Підсилення входу видалено разом із чипом: команда не підтримується (400
            // not_supported). Тип лишається в WebCmdType (ABI), стан не змінюється.
            why = "not_supported";
            return WebCmdResult::Unsupported;

        case WebCmdType::StandbySet:
            if (c.flag) {
                if (s_mode != Mode::Standby) enterStandby();
            } else if (s_mode == Mode::Standby) {
                leaveStandby();
            }
            break;

        case WebCmdType::StandbyToggle:
            togglePower();
            break;

        case WebCmdType::InputSet:
            if (c.value < 0 || c.value >= defaults::kInputCount) {
                why = "input_unavailable";  // поза 0..2
                return WebCmdResult::OutOfRange;
            }
            // [Prompt 39] Вхід вимкнено користувачем (маска) — не помилка значення, а стан
            // налаштувань: 409 input_disabled (на відміну від апаратно відсутнього входу -> 400).
            if (!inputEnabled(static_cast<uint8_t>(c.value))) {
                why = "input_disabled";
                return WebCmdResult::NotAllowed;
            }
            if (!inputAvailable(static_cast<uint8_t>(c.value))) {  // [Prompt 28]: офлайн -> без радіо
                why = "input_unavailable";
                return WebCmdResult::OutOfRange;
            }
            if (s_mode == Mode::Menu) closeStationList();
            changeInput(static_cast<uint8_t>(c.value));
            break;

        case WebCmdType::InputsEnabledSet: {
            // [Prompt 39] value = нова маска; нормалізується тут ще раз (біт радіо = 1, зайві біти
            // скинуто) — валідацію запиту робить веб-обробник, тут лише страховка.
            const uint8_t want =
                settings_cfg::normalizeInputMask(static_cast<uint8_t>(c.value & 0xFF));
            if (want == s_inputMask) {
                break;  // нічого не змінилось: ні запису в NVS, ні перемикань
            }
            APP_LOG("inputs enabled mask 0x%02X -> 0x%02X (WEB)\n",
                    static_cast<unsigned>(s_inputMask), static_cast<unsigned>(want));
            s_inputMask = want;
            uint8_t maskToStore = want;  // modify() приймає void* ctx (fn викликається синхронно)
            SettingsStore::modify(applyMaskPersist, &maskToStore);  // NVS з дебаунсом; лише при зміні
            if (!inputAvailable(s_input)) {
                // Активний вхід щойно вимкнено (або lastInput у Standby): на перший доступний (радіо).
                const uint8_t fb = firstAvailableInput();
                APP_LOG("active input %u disabled -> input %u\n", static_cast<unsigned>(s_input),
                        static_cast<unsigned>(fb));
                if (s_mode == Mode::Standby) {
                    // Standby: чіп і потік не чіпаємо (профіль застосує leaveStandby()); лише
                    // тримаємо s_input / профіль / AppState узгодженими.
                    captureActiveProfile();
                    s_input = fb;
                    loadActiveFromProfile(fb);
                    s_persistNeeded = true;
                } else {
                    if (s_mode == Mode::Menu) closeStationList();
                    changeInput(fb);  // мʼют -> профіль -> розмʼют, зупинка/запуск потоку; persist
                }
            }
            break;
        }

        case WebCmdType::PlayerPlay:
        case WebCmdType::PlayerPause:
        case WebCmdType::PlayerToggle: {
            if (!webRadioGate(why)) return WebCmdResult::NotAllowed;
            const bool active = playerActive();
            const bool wantPlay = (c.type == WebCmdType::PlayerPlay) ||
                                  (c.type == WebCmdType::PlayerToggle && !active);
            if (wantPlay) {
                if (stationCount() == 0) {
                    why = "no_stations";
                    return WebCmdResult::NotAllowed;
                }
                if (!active) startStream();  // уже грає -> нічого не робимо
            } else if (active) {
                pauseRadio();  // [Prompt 47a] плавний мʼют -> stop()
            }
            break;
        }

        case WebCmdType::StationPlay: {
            const int n = stationCount();
            if (n == 0) {
                why = "no_stations";
                return WebCmdResult::NotAllowed;
            }
            if (c.value < 0 || c.value >= n) {
                why = "station_out_of_range";
                return WebCmdResult::OutOfRange;
            }
            const uint16_t idx = static_cast<uint16_t>(c.value);
            if (s_offline) {  // [Prompt 28] радіо недоступне (веб в офлайні й так не працює)
                why = "input_unavailable";
                return WebCmdResult::OutOfRange;
            }
            if (s_input != 0) {
                if (!c.flag) {
                    why = "not_radio_input";
                    return WebCmdResult::NotAllowed;
                }
                if (WifiManager::isApMode()) {
                    why = "wifi_setup";
                    return WebCmdResult::NotAllowed;
                }
                s_station = idx;  // changeInput(0) сам запустить потік зі s_station
                changeInput(0);
            } else {
                if (s_mode == Mode::WifiSetup) {
                    why = "wifi_setup";
                    return WebCmdResult::NotAllowed;
                }
                if (s_mode == Mode::Menu) closeStationList();
                // Та сама станція вже грає — нічого не робимо; на паузі — запускаємо
                // (так само, як selectFromList()).
                if (idx != s_station || AudioPlayer::state() == PlayerState::Idle) {
                    changeStation(idx);
                }
            }
            break;
        }

        case WebCmdType::StationNext:
        case WebCmdType::StationPrev:
            if (!webRadioGate(why)) return WebCmdResult::NotAllowed;
            if (stationCount() == 0) {
                why = "no_stations";
                return WebCmdResult::NotAllowed;
            }
            if (s_mode == Mode::Menu) closeStationList();
            stepStation(c.type == WebCmdType::StationNext ? cfg::kStationRightStep
                                                          : cfg::kStationLeftStep);
            break;
    }

    publishState();
    if (s_persistNeeded) {
        persist();
        s_persistNeeded = false;
    }
    return WebCmdResult::Ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// Публічний API
// ---------------------------------------------------------------------------
void AppController::handleEvent(const Event& event) {
    if (s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return;
    }
    handleLocked(event);
    xSemaphoreGiveRecursive(s_lock);
}

// [Prompt 13] ДОДАНО
bool AppController::setTone(ToneUpdate& u) {
    u.bassOk = u.trebleOk = u.balanceOk = u.loudnessOk = false;
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return false;
    }
    // [Prompt 48a] Тембр / баланс / loudness не підтримуються (чипа немає; баланс повернеться цифровим у P48b
    // іншим шляхом): усі *Ok = false («Unsupported»), стан не змінюється. Метод лишено заради ABI.
    if (u.hasBass || u.hasTreble || u.hasBalance || u.hasLoudness) {
        APP_LOG("tone/balance/loudness (WEB) unsupported: ignored\n");
    }
    xSemaphoreGiveRecursive(s_lock);
    return true;
}

// [Prompt 15] ДОДАНО: навчання IR
bool AppController::beginIrLearn(Action target) {
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return false;
    }
    const bool ok = beginIrLearnLocked(target);
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

void AppController::cancelIrLearn() {
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return;
    }
    cancelIrLearnLocked();
    xSemaphoreGiveRecursive(s_lock);
}

bool AppController::confirmIrOverwrite() {
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return false;
    }
    const bool ok = confirmIrOverwriteLocked();
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

// [Prompt 16] ДОДАНО: OTA
bool AppController::beginOta() {
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return false;
    }
    bool ok = false;
    if (s_mode != Mode::IrLearn && s_mode != Mode::OtaUpdate && !restarting()) {  // [Prompt 28]
        s_otaPrevMode = s_mode;
        s_otaAbortPending = false;
        publishOtaProgress(0);  // спершу 0, потім режим: екран не покаже старий відсоток
        s_mode = Mode::OtaUpdate;
        s_phase = Phase::None;  // Settle скасовано
        applyHardwareMute(true);  // desiredMute() уже true; мʼют ПЕРЕД зупинкою потоку ([P47a]: з очікуванням згасання)
        stopStream();
        lineRelease();  // [Prompt 47c] звільнити RAM проходу (≈ 21 КБ) для OTA; мʼют — desiredMute()
        publishState();
        APP_LOG("OTA begin, mode %s -> OtaUpdate\n", modeName(s_otaPrevMode));
        ok = true;
    }
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

void AppController::setOtaProgress(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    // Читання без мʼютекса: хибне значення безпечне (пропущений чи зайвий кадр).
    if (!s_started || s_mode != Mode::OtaUpdate || percent == s_otaPubPercent) {
        return;
    }
    publishOtaProgress(percent);
}

void AppController::otaFailed(const char* reason) {
    APP_LOG("OTA failed: %s\n", reason != nullptr ? reason : "?");
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        s_otaAbortPending = true;
        return;
    }
    leaveOtaLocked();
    xSemaphoreGiveRecursive(s_lock);
}

// [Prompt 18] ДОДАНО: команди з вебу
WebCmdResult AppController::runWebCommand(const WebCommand& cmd, const char** reasonOut) {
    if (reasonOut != nullptr) {
        *reasonOut = nullptr;
    }
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        if (reasonOut != nullptr) {
            *reasonOut = "busy";
        }
        return WebCmdResult::Busy;
    }
    const char* why = nullptr;
    const WebCmdResult r = execWebCommandLocked(cmd, why);
    xSemaphoreGiveRecursive(s_lock);
    if (reasonOut != nullptr) {
        *reasonOut = why;
    }
    return r;
}

// [Prompt 28] ДОДАНО: офлайн-режим
bool AppController::startOffline() {
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return false;
    }
    const bool ok = startOfflineLocked();
    if (ok) {
        publishState();
        if (s_persistNeeded) {
            persist();
            s_persistNeeded = false;
        }
    }
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

// [Prompt 47c] ДОДАНО: живий пресет EQ поточного зовнішнього входу (викликає веб-обробник /api/eq)
bool AppController::applyLineEq(uint8_t input, const int8_t gains[eq_cfg::kBandCount]) {
#if LINE_PATH_ENABLE
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
        return false;
    }
    if (input != 0 && input == s_input && LinePath::isReady()) {
        LinePath::setEqAllDb(gains);  // неблокуюче; діє й до старту проходу
        APP_LOG("line EQ preset applied live (input %u)\n", static_cast<unsigned>(input));
    }
    xSemaphoreGiveRecursive(s_lock);
#else
    (void)input;
    (void)gains;
#endif
    return true;
}

bool AppController::begin() {
    if (s_started) {
        return true;
    }

    // ПЕРШИМ ділом: до цього AppState::begin() ніхто не викликав.
    if (!AppState::begin()) {
        Serial.println("[APP] AppState::begin failed");
        return false;
    }
    // [Prompt 11] StationStore не залежить від AppState (порядок байдужий), але
    // МАЄ бути готовий до читання Settings.lastStation нижче (обрізання індексу
    // за stationCount()). Збій не фатальний: без станцій Radio просто мовчить.
    if (!StationStore::begin()) {
        Serial.println("[APP] StationStore::begin failed or empty: no stations");
    }
    if (!EventBus::isReady()) {
        Serial.println("[APP] EventBus is not ready");
        return false;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == nullptr) {
        Serial.println("[APP] mutex create failed");
        return false;
    }

    // Налаштування — лише з Settings; гучність обрізаємо за audio_cfg::kVolumeUiMin..kVolumeUiMax
    // (Settings діапазон гучності не валідує).
    // [Prompt 21b/46a] Гучності профілів усіх входів — у таблицю контролера, робоча гучність — з профілю
    // останнього активного входу. Пресети EQ профілів лишаються в Settings (контролер їх не копіює).
    const Settings st = SettingsStore::snapshot();
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        s_volumes[i] = st.profiles[i].volume;
    }
    // [Prompt 39] Маска входів — ДО вибору s_input: lastInput, вимкнений (або апаратно відсутній),
    // замінюється на перший доступний вхід (радіо).
    s_inputMask = settings_cfg::normalizeInputMask(st.inputEnabledMask);
    s_input = inputAvailable(st.lastInput) ? st.lastInput : firstAvailableInput();
    s_station = (st.lastStation < stationCount()) ? st.lastStation : 0;
    loadActiveFromProfile(s_input);  // [P47a] також виставляє цифрове послаблення радіо (DSP у мʼюті)
    s_userMute = st.lastMute;
    s_target = AdjustTarget::Volume;
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    s_phase = Phase::None;
    s_mode = Mode::Standby;  // до завершення старту — тиша (desiredMute() == true)

    // Спершу тиша, потім решта. [P47a] Цифровий мʼют радіо (DSP стартує замʼюченим) + чип.
    s_hwMute = false;
    applyHardwareMute();

    // [Prompt 39] Причина скидання: подача живлення / brownout (cfg::kColdStartResetReasons) —
    // стартуємо у Standby, як після POWER. Інші причини (ESP.restart(), паніка, watchdog, ...) —
    // як раніше (kBootInStandby = false -> увімкнутись і відновити lastInput/lastStation).
    const esp_reset_reason_t resetReason = esp_reset_reason();
    const bool coldStart = cfg::kStandbyOnColdStart && cfg::isColdStartReason(resetReason);
    if (coldStart) {
        APP_LOG("cold start (reset reason %d) -> standby\n", static_cast<int>(resetReason));
    } else {
        APP_LOG("reset reason %d (not a cold start)\n", static_cast<int>(resetReason));
    }

    if (cfg::kBootInStandby || coldStart) {
        APP_LOG("boot in standby\n");
        // [Prompt 48a] Реле не чіпаємо (лишається в TV Box після input_selector::begin()), тиша: профіль
        // входу застосується повністю при виході зі standby.
    } else {
        powerOnTransition();  // режим, реле / старт проходу + профіль, playUrl для Radio, розмʼют у tick()
    }
    publishState();

    BaseType_t ok = xTaskCreatePinnedToCore(taskMain, "app_ctrl", cfg::kTaskStackBytes, nullptr,
                                            cfg::kTaskPriority, nullptr, cfg::kTaskCore);
    if (ok != pdPASS) {
        Serial.println("[APP] task create failed");
        return false;
    }

    s_started = true;
    APP_LOG("ready: mode=%s input=%u station=%u vol=%d mute=%d inputMask=0x%02X\n", modeName(s_mode),
            static_cast<unsigned>(s_input), static_cast<unsigned>(s_station),
            static_cast<int>(s_volume), s_userMute ? 1 : 0, static_cast<unsigned>(s_inputMask));
    return true;
}