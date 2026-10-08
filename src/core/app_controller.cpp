// AppController: центральний координатор (Prompt 8).
//
// Усі переходи з очікуванням (мʼют -> зміна -> розмʼют -> ramp) реалізовано як
// невеликий автомат Phase, який рухає tick() за збереженим часом, а не
// блокуючими затримками. Один внутрішній рекурсивний мʼютекс захищає і
// handleEvent(), і tick().
//
// [Prompt 11] Список станцій береться зі StationStore (LittleFS), а не з
// тестових констант плеєра.
//
// [Prompt 12] Стан Wi-Fi береться з WifiManager (прямі виклики WiFi.* прибрано).
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

#include "core/app_controller.h"

#include <Arduino.h>
#include <string.h>

#include "audio/amp_standby.h"  // [Prompt 23c]
#include "audio/audio_player.h"
#include "audio/audio_processor.h"
#include "config/amp_config.h"  // [Prompt 23c]
#include "config/app_controller_config.h"
#include "config/defaults.h"
#include "config/features.h"  // [Prompt 23c]
#include "config/ir_learn_ui_config.h"  // [Prompt 15] ДОДАНО
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
enum class Phase : uint8_t {
    None,    // переходу немає
    Settle,  // атенюатори замʼючені, чекаємо kUnmuteDelayMs
    Ramp,    // розмʼючено, гучність плавно росте до цілі
};

constexpr uint8_t kAdjustTargetCount = 5;  // Volume..Gain

SemaphoreHandle_t s_lock = nullptr;
bool s_started = false;
AudioProcessor* s_proc = nullptr;
AudioProcessorCapabilities s_caps = {};

Mode s_mode = Mode::Standby;
uint8_t s_input = 0;
uint16_t s_station = 0;
int8_t s_volume = 0;  // ЦІЛЬОВА гучність
int8_t s_bass = 0;
int8_t s_treble = 0;
int8_t s_balance = 0;
int8_t s_gain = 0;
bool s_loudness = false;  // [Prompt 21b]
bool s_userMute = false;
// [Prompt 21b] Профілі всіх логічних входів (включно з входом 3, якого PT2313L не має).
InputProfile s_profiles[defaults::kInputCount] = {};
AdjustTarget s_target = AdjustTarget::Volume;
uint32_t s_lastEncMs = 0;  // [Prompt 31] millis() останньої події енкодера
// [Prompt 32] Подія «показати спливне вікно параметра»: що показувати і лічильник подій.
// Лічильник росте лише на ЛОКАЛЬНІ зміни (handleLocked); веб-команди (runWebCommand) його не чіпають.
AdjustTarget s_popupTarget = AdjustTarget::Volume;
uint8_t s_popupSeq = 0;
MenuContext s_menuCtx = MenuContext::None;
uint16_t s_menuSel = 0;

Phase s_phase = Phase::None;
uint32_t s_phaseStartMs = 0;
int16_t s_rampCur = 0;
uint32_t s_rampLastStepMs = 0;

bool s_gainHoldActive = false;
uint32_t s_gainHoldUntilMs = 0;

bool s_hwMute = false;  // що востаннє УСПІШНО надіслано в чіп
uint32_t s_lastMuteTryMs = 0;

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

const char* targetName(AdjustTarget t) {
    switch (t) {
        case AdjustTarget::Volume:  return "volume";
        case AdjustTarget::Bass:    return "bass";
        case AdjustTarget::Treble:  return "treble";
        case AdjustTarget::Balance: return "balance";
        case AdjustTarget::Gain:    return "gain";
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

// --- Можливості ------------------------------------------------------------
void loadCaps() {
    if (s_proc != nullptr) {
        s_caps = s_proc->capabilities();
        if (s_caps.inputCount == 0 || s_caps.inputCount > defaults::kInputCount) {
            s_caps.inputCount = defaults::kInputCount;
        }
        return;
    }
    // Без процесора: умовні межі, щоб логіка стану не ламалась.
    s_caps = {};
    s_caps.inputCount = defaults::kInputCount;
    s_caps.volumeMin = 0;
    s_caps.volumeMax = 100;
}

bool targetSupported(AdjustTarget t) {
    switch (t) {
        case AdjustTarget::Volume:  return true;
        case AdjustTarget::Bass:    return s_caps.bass;
        case AdjustTarget::Treble:  return s_caps.treble;
        case AdjustTarget::Balance: return s_caps.balance;
        case AdjustTarget::Gain:    return s_caps.inputGain && s_caps.gainMax > s_caps.gainMin;
    }
    return false;
}

// --- [Prompt 21b] Профілі входів ---------------------------------------------
bool gainSupported() {
    return s_caps.inputGain && s_caps.gainMax > s_caps.gainMin;
}

// Робочі значення поточного входу -> s_profiles[s_input]. Параметри, яких чип не підтримує,
// у профілі НЕ затираються (робоча копія там 0/false).
void captureActiveProfile() {
    if (s_input >= defaults::kInputCount) {
        return;
    }
    InputProfile& p = s_profiles[s_input];
    p.volume = s_volume;
    if (s_caps.bass) p.bass = s_bass;
    if (s_caps.treble) p.treble = s_treble;
    if (s_caps.balance) p.balance = s_balance;
    if (gainSupported()) p.gain = s_gain;
    if (s_caps.loudness) p.loudness = s_loudness;
}

// s_profiles[idx] -> робочі значення, обрізані за capabilities() поточного чипа.
// Чипа не торкається й AppState не публікує (це робить викликач).
void loadActiveFromProfile(uint8_t idx) {
    if (idx >= defaults::kInputCount) {
        idx = 0;
    }
    const InputProfile& p = s_profiles[idx];
    s_volume = static_cast<int8_t>(clampInt(p.volume, s_caps.volumeMin, s_caps.volumeMax));
    s_bass = s_caps.bass ? static_cast<int8_t>(clampInt(p.bass, s_caps.toneMin, s_caps.toneMax))
                         : 0;
    s_treble = s_caps.treble
                   ? static_cast<int8_t>(clampInt(p.treble, s_caps.toneMin, s_caps.toneMax))
                   : 0;
    s_balance = s_caps.balance ? static_cast<int8_t>(clampInt(p.balance, s_caps.balanceMin,
                                                              s_caps.balanceMax))
                               : 0;
    s_gain = gainSupported()
                 ? static_cast<int8_t>(clampInt(p.gain, s_caps.gainMin, s_caps.gainMax))
                 : 0;
    s_loudness = s_caps.loudness ? p.loudness : false;
}

// Надсилає в чип робочі значення (БЕЗ гучності: її піднімає ramp). Викликати лише під
// мʼютом атенюаторів. withGain=true — лише ПІСЛЯ setInput(), бо setGain() стосується
// поточного входу чипа; без setInput() (завантаження в standby) gain не чіпаємо.
void applyActiveToChip(bool withGain) {
    if (s_proc == nullptr) {
        return;
    }
    bool ok = true;
    if (s_caps.bass) ok = s_proc->setBass(s_bass) && ok;
    if (s_caps.treble) ok = s_proc->setTreble(s_treble) && ok;
    if (s_caps.balance) ok = s_proc->setBalance(s_balance) && ok;
    if (withGain && gainSupported()) ok = s_proc->setGain(s_gain) && ok;
    if (s_caps.loudness) ok = s_proc->setLoudness(s_loudness) && ok;
    if (!ok) {
        APP_LOG("profile apply: some I2C writes FAILED\n");
    }
    APP_LOG("profile in%u: vol=%d bass=%d treble=%d bal=%d gain=%d loud=%d\n",
            static_cast<unsigned>(s_input), static_cast<int>(s_volume), static_cast<int>(s_bass),
            static_cast<int>(s_treble), static_cast<int>(s_balance), static_cast<int>(s_gain),
            s_loudness ? 1 : 0);
}

// --- Мʼют атенюаторів ------------------------------------------------------
// [Prompt 15] «Логічний» Standby: Standby АБО навчання IR, розпочате зі Standby. Без цього
// вхід у IrLearn зі Standby зняв би мʼют атенюаторів (desiredMute() бачив би
// mode != Standby) і зовнішній вхід заграв би на «вимкненому» пристрої.
bool logicallyStandby() {
    return s_mode == Mode::Standby ||
           (s_mode == Mode::IrLearn && s_irPrevMode == Mode::Standby);
}

bool desiredMute() {
    return s_userMute || logicallyStandby() || s_phase == Phase::Settle || s_gainHoldActive ||
           s_mode == Mode::OtaUpdate ||  // [Prompt 16]: під час прошивки тиша
           restarting() ||               // [Prompt 28]: перезапуск — тиша до кінця
           s_mode == Mode::WifiSetup;    // [Prompt 28]: екран налаштування Wi-Fi — тиша
}

void applyHardwareMute(bool force = false) {
    const bool want = desiredMute();
    if (s_proc == nullptr) {
        s_hwMute = want;
        return;
    }
    if (!force && want == s_hwMute) {
        return;
    }
    s_lastMuteTryMs = millis();
    if (s_proc->setMute(want)) {
        s_hwMute = want;
        APP_LOG("hw mute %s\n", want ? "on" : "off");
    } else {
        APP_LOG("hw mute %s FAILED (I2C), will retry\n", want ? "on" : "off");
    }
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
    int8_t volume, bass, treble, balance, gain;
    bool mute;
    uint16_t station;
    AdjustTarget target;
    MenuContext menuCtx;
    uint16_t menuSel;
    bool loudness;  // [Prompt 21b]
    bool offline;     // [Prompt 28]
    bool restarting;  // [Prompt 28]
    AdjustTarget popupTarget;  // [Prompt 32]
    uint8_t popupSeq;          // [Prompt 32]
    char inputName[kInputNameMax];  // [Prompt 23b]
};

void applyPub(AppStateData& s, void* c) {
    const PubCtx* p = static_cast<const PubCtx*>(c);
    s.mode = p->mode;
    s.inputIndex = p->input;
    s.volume = p->volume;
    s.bass = p->bass;
    s.treble = p->treble;
    s.balance = p->balance;
    s.gain = p->gain;
    s.mute = p->mute;
    s.stationIndex = p->station;
    s.adjustTarget = p->target;
    s.menuContext = p->menuCtx;
    s.menuSelection = p->menuSel;
    s.loudness = p->loudness;  // [Prompt 21b]
    s.offline = p->offline;        // [Prompt 28]
    s.restarting = p->restarting;  // [Prompt 28]
    s.popupTarget = p->popupTarget;  // [Prompt 32]
    s.popupSeq = p->popupSeq;        // [Prompt 32]
    memcpy(s.inputName, p->inputName, sizeof(s.inputName));  // [Prompt 23b]
}

void publishState() {
    PubCtx p = {s_mode,   s_input, s_volume, s_bass,    s_treble, s_balance,
                s_gain,   s_userMute, s_station, s_target, s_menuCtx, s_menuSel,
                s_loudness, s_offline, restarting(), s_popupTarget, s_popupSeq};
    resolveInputName(s_input, p.inputName, sizeof(p.inputName));  // [Prompt 23b]
    AppState::modify(applyPub, &p);
}

// [Prompt 21b] Єдине місце запису в Settings: lastInput/lastStation/lastMute + ВСІ профілі
// входів (профіль поточного входу перед цим оновлюється з робочих значень).
struct PersistCtx {
    uint8_t input;
    uint16_t station;
    bool mute;
    const InputProfile* profiles;
};

static_assert(sizeof(s_profiles) == sizeof(Settings::profiles),
              "controller profile table must mirror Settings::profiles");

void applyPersist(Settings& s, void* c) {
    const PersistCtx* p = static_cast<const PersistCtx*>(c);
    s.lastInput = p->input;
    s.lastStation = p->station;
    s.lastMute = p->mute;
    memcpy(s.profiles, p->profiles, sizeof(s.profiles));
}

// SettingsStore::modify() сама позначає кеш «брудним» (як requestSave()):
// фактичний запис відбудеться з дебаунсом (не на кожен крок енкодера).
void persist() {
    captureActiveProfile();
    PersistCtx p = {s_input, s_station, s_userMute, s_profiles};
    SettingsStore::modify(applyPersist, &p);
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

// --- Гучність і ramp -------------------------------------------------------
// Змінює ЦІЛЬОВУ гучність; у чіп іде одразу, якщо немає переходу. Під час
// Settle чіп лишається на volumeMin (піднімемо ramp-ом), під час Ramp росте
// далі до нової цілі (а якщо ціль нижча за поточний рівень чіпа — опускаємо).
bool setVolumeTarget(int v) {
    v = clampInt(v, s_caps.volumeMin, s_caps.volumeMax);
    if (v == s_volume) {
        return false;
    }
    s_volume = static_cast<int8_t>(v);
    if (s_phase == Phase::Ramp) {
        if (v < s_rampCur) {
            s_rampCur = static_cast<int16_t>(v);
            if (s_proc != nullptr) s_proc->setVolume(static_cast<int8_t>(v));
        }
    } else if (s_phase == Phase::None) {
        if (s_proc != nullptr) s_proc->setVolume(static_cast<int8_t>(v));
    }
    return true;
}

// Початок переходу: мʼют ПЕРШИМ, потім гучність у мінімум (ramp підніме її
// після розмʼюту). Якщо перехід уже йде — просто перезапускає таймер.
void startTransition() {
    s_phase = Phase::Settle;
    s_phaseStartMs = millis();
    applyHardwareMute();
    if (s_proc != nullptr) {
        s_proc->setVolume(s_caps.volumeMin);
    }
}

// Увімкнення: режим за s_input, перехід, вхід у чіпі, запуск потоку для Radio.
// Вхід/станцію викликач уже поклав у s_input/s_station.
void powerOnTransition() {
    s_mode = (s_input == 0) ? Mode::Radio : Mode::ExternalInput;
    s_target = AdjustTarget::Volume;  // [Prompt 31] після вмикання/виходу зі standby — гучність
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    startTransition();
    ampRequestOn();  // [Prompt 23c]: після мʼюту; прогрів іде паралельно з налаштуванням чипа
    // [Prompt 21b] Усе ще під мʼютом: профіль входу -> робочі значення, вхід у чіп, потім
    // тембр/баланс/gain/loudness цього входу. Гучність підніме ramp у tick().
    loadActiveFromProfile(s_input);
    if (s_proc != nullptr) {
        s_proc->setInput(s_input);
        applyActiveToChip(true);
    }
    if (s_input == 0) {
        startStream();
    }
    publishStationName();  // [Prompt 20b]
}

// --- Дії -------------------------------------------------------------------
void enterStandby() {
    APP_LOG("standby on\n");
    s_phase = Phase::None;
    s_gainHoldActive = false;
    s_target = AdjustTarget::Volume;  // [Prompt 31]
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    s_mode = Mode::Standby;
    applyHardwareMute();  // Standby входить у desiredMute(): негайно, без ramp
    ampRequestOff();      // [Prompt 23c]: пін LOW через kAmpOffDelayMs (у tick())
    stopStream();
    persist();
    SettingsStore::flush();
    s_persistNeeded = false;
}

// [Prompt 28] ЄДИНЕ місце рішення «чи доступний вхід»: індекс у межах чипа (PT2313L = 3 входи) і,
// в офлайні, не радіо (вхід 0). Усі вибори входу (кнопки, пульт, веб, вихід зі standby) йдуть
// через цю функцію.
bool inputAvailable(uint8_t idx) {
    return idx < s_caps.inputCount && !(s_offline && idx == 0);
}

// Перший доступний вхід за порядком 0..inputCount-1 (в офлайні це перший не-радіо вхід).
uint8_t firstAvailableInput() {
    for (uint8_t i = 0; i < s_caps.inputCount; ++i) {
        if (inputAvailable(i)) {
            return i;
        }
    }
    return 0;  // недосяжно: inputCount >= 3 на обох чипах
}

void leaveStandby() {
    captureActiveProfile();  // [Prompt 21b] профіль «старого» входу, поки s_input ще його
    const Settings st = SettingsStore::snapshot();
    s_input = (st.lastInput < s_caps.inputCount) ? st.lastInput : 0;
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
    applyHardwareMute();
    s_persistNeeded = true;
    APP_LOG("mute %s\n", s_userMute ? "on" : "off");
}

void changeInput(uint8_t newIdx) {
    if (newIdx == s_input) {
        return;
    }
    if (!inputAvailable(newIdx)) {  // [Prompt 28] сторожа: у офлайні радіо недоступне з будь-якого шляху
        APP_LOG("input %u unavailable (%s)\n", static_cast<unsigned>(newIdx),
                s_offline ? "offline" : "no such input");
        return;
    }
    const bool leavingRadio = (s_input == 0 && newIdx != 0);
    const bool enteringRadio = (s_input != 0 && newIdx == 0);
    APP_LOG("input %u -> %u (%s)\n", static_cast<unsigned>(s_input),
            static_cast<unsigned>(newIdx),
            newIdx < defaults::kInputCount ? defaults::kInputNames[newIdx] : "?");

    captureActiveProfile();  // [Prompt 21b] профіль старого входу фіксуємо ДО зміни s_input
    s_input = newIdx;
    s_mode = (newIdx == 0) ? Mode::Radio : Mode::ExternalInput;
    startTransition();  // мʼют -> гучність у мінімум
    if (leavingRadio) {
        stopStream();
    }
    // [Prompt 21b] Під мʼютом: профіль нового входу -> робочі значення (s_volume = ціль
    // майбутнього ramp), setInput(), потім setGain() та решта (порядок гарантує, що gain
    // нового входу = значення профілю, а не те, що підставив драйвер).
    loadActiveFromProfile(newIdx);
    if (s_proc != nullptr) {
        s_proc->setInput(newIdx);
        applyActiveToChip(true);
    }
    if (enteringRadio) {
        startStream();
    }
    publishStationName();  // [Prompt 20b]: вхід Radio -> назва зі списку, інакше порожньо
    s_persistNeeded = true;
}

void stepInput(int dir) {
    const int count = s_caps.inputCount;  // PT2313L = 3: вхід 3 пропускається
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
        APP_LOG("input %u unavailable (%s)\n", static_cast<unsigned>(idx),
                s_offline && idx == 0 ? "offline" : "not on this processor");
        return;
    }
    changeInput(idx);
}

// [Prompt 28] Запуск офлайн-режиму. Лише з Mode::WifiSetup; викликати під s_lock. Порядок:
// 1) s_offline (радіо одразу стає недоступним), 2) вимкнення Wi-Fi (неблокуюче, виконує задача
// WifiManager), 3) перший не-радіо вхід через changeInput() — мʼют, профіль входу, розмʼют і ramp
// як при звичайній зміні входу (підсилювач у WifiSetup уже працює: прогріву немає).
bool startOfflineLocked() {
    if (s_offline || restarting() || s_mode != Mode::WifiSetup) {
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
        // вручну (мʼют -> ramp, профіль входу вже застосований).
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
    startTransition();
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
    // Мʼют атенюаторів не потрібен: XSMT ЦАП керує сам плеєр.
    if (AudioPlayer::state() != PlayerState::Idle || s_playPending) {
        stopStream();
        APP_LOG("pause\n");
    } else {
        char nm[sizeof(Station::name)];
        stationLabel(s_station, nm, sizeof(nm));
        APP_LOG("play %s\n", nm);
        startStream();
    }
}

void cycleTarget() {
    for (uint8_t i = 1; i <= kAdjustTargetCount; ++i) {
        const AdjustTarget t = static_cast<AdjustTarget>(
            (static_cast<uint8_t>(s_target) + i) % kAdjustTargetCount);
        if (targetSupported(t)) {
            s_target = t;
            break;
        }
    }
    APP_LOG("target -> %s\n", targetName(s_target));
}

// Gain — апаратний стрибок рівня: мʼют ПЕРЕД командою, розмʼют через
// kGainMuteHoldMs (його знімає tick()).
void adjustGain(int d) {
    if (!targetSupported(AdjustTarget::Gain) || s_proc == nullptr) {
        return;
    }
    const int cur = s_gain;  // [Prompt 21b] джерело правди — профіль/робоча копія, не драйвер
    const int nv = clampInt(cur + d, s_caps.gainMin, s_caps.gainMax);
    if (nv == cur) {
        return;
    }
    s_gainHoldActive = true;
    s_gainHoldUntilMs = millis() + cfg::kGainMuteHoldMs;
    applyHardwareMute();
    if (s_proc->setGain(static_cast<int8_t>(nv))) {
        s_gain = static_cast<int8_t>(nv);
        s_persistNeeded = true;  // [Prompt 21b] gain тепер зберігається в профілі входу
        APP_LOG("gain=%d (mute hold)\n", static_cast<int>(s_gain));
    } else {
        APP_LOG("gain=%d FAILED (I2C), unchanged\n", nv);
    }
}

// Один «крок» цілі t у напрямку знака d (|d| = кількість detent-ів).
void adjustParam(AdjustTarget t, int d) {
    switch (t) {
        case AdjustTarget::Volume:
            if (setVolumeTarget(static_cast<int>(s_volume) + d * cfg::kVolumeStep)) {
                s_persistNeeded = true;
                APP_LOG("volume=%d\n", static_cast<int>(s_volume));
            }
            break;
        case AdjustTarget::Bass:
            if (s_caps.bass) {
                const int nv = clampInt(s_bass + d, s_caps.toneMin, s_caps.toneMax);
                if (nv != s_bass) {
                    s_bass = static_cast<int8_t>(nv);
                    if (s_proc != nullptr) s_proc->setBass(s_bass);
                    s_persistNeeded = true;
                    APP_LOG("bass=%d\n", static_cast<int>(s_bass));
                }
            }
            break;
        case AdjustTarget::Treble:
            if (s_caps.treble) {
                const int nv = clampInt(s_treble + d, s_caps.toneMin, s_caps.toneMax);
                if (nv != s_treble) {
                    s_treble = static_cast<int8_t>(nv);
                    if (s_proc != nullptr) s_proc->setTreble(s_treble);
                    s_persistNeeded = true;
                    APP_LOG("treble=%d\n", static_cast<int>(s_treble));
                }
            }
            break;
        case AdjustTarget::Balance:
            if (s_caps.balance) {
                const int nv = clampInt(s_balance + d, s_caps.balanceMin, s_caps.balanceMax);
                if (nv != s_balance) {
                    s_balance = static_cast<int8_t>(nv);
                    if (s_proc != nullptr) s_proc->setBalance(s_balance);
                    s_persistNeeded = true;
                    APP_LOG("balance=%d\n", static_cast<int>(s_balance));
                }
            }
            break;
        case AdjustTarget::Gain:
            adjustGain(d);
            break;
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
        case Action::INPUT_AUX:   if (plain) selectInput(3); break;
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
// лишається (desiredMute()), потік не потрібен. Інакше — мʼют, volumeMin, ramp у tick().
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
        if (s_input == 0 && (s_mode == Mode::Radio || s_mode == Mode::Menu)) {
            startStream();  // сам відкладе, якщо немає Wi-Fi
        }
    }
    publishState();
    publishOtaProgress(0);
}

// --- [Prompt 28] «Тихий» перезапуск -------------------------------------------
// Викликати лише під s_lock. Послідовність (без блокувань): мʼют атенюаторів (desiredMute() уже
// true через restarting()) -> стоп потоку (плеєр сам мʼютить XSMT) -> ampRequestOff() (пін LOW
// через kAmpOffDelayMs, P23c) -> restartService() чекає LOW, паузу kRestartMuteMs і ESP.restart().
void beginRestart() {
    if (restarting()) {
        return;
    }
    APP_LOG("restart: POWER held, quiet restart\n");
    s_phase = Phase::None;  // Settle/Ramp скасовано (як у beginOta)
    s_gainHoldActive = false;
    s_playPending = false;
    s_restartPhase = RestartPhase::WaitAmp;
    s_restartDeadlineMs = millis() + cfg::kRestartMaxWaitMs;
    applyHardwareMute();  // мʼют ПЕРШИМ (негайно, без ramp)
    if (AudioPlayer::state() != PlayerState::Idle) {
        stopStream();  // плеєр мʼютить XSMT ЦАП
    } else {
        s_playUrlValid = false;
    }
    ampRequestOff();  // ПІСЛЯ applyHardwareMute(), як у enterStandby()
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
// Енкодер: обертання й коротка клавіша показують ПОТОЧНУ ціль (після cycleTarget — нову).
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
        case Action::BASS_UP:
        case Action::BASS_DOWN:    notifyPopup(AdjustTarget::Bass);    break;
        case Action::TREBLE_UP:
        case Action::TREBLE_DOWN:  notifyPopup(AdjustTarget::Treble);  break;
        case Action::BALANCE_UP:
        case Action::BALANCE_DOWN: notifyPopup(AdjustTarget::Balance); break;
        case Action::GAIN_UP:
        case Action::GAIN_DOWN:    notifyPopup(AdjustTarget::Gain);    break;
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

    // [Prompt 31] Активність енкодера (обертання, клік, утримання) перезапускає відлік
    // автоповернення цілі на гучність. Інші джерела (кнопки, IR) відлік не чіпають.
    if (e.action == Action::ENC_CW || e.action == Action::ENC_CCW ||
        e.action == Action::ENC_PRESS) {
        s_lastEncMs = millis();
    }

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
            case Action::BASS_UP:      adjustParam(AdjustTarget::Bass, +1); break;
            case Action::BASS_DOWN:    adjustParam(AdjustTarget::Bass, -1); break;
            case Action::TREBLE_UP:    adjustParam(AdjustTarget::Treble, +1); break;
            case Action::TREBLE_DOWN:  adjustParam(AdjustTarget::Treble, -1); break;
            case Action::BALANCE_UP:   adjustParam(AdjustTarget::Balance, +1); break;
            case Action::BALANCE_DOWN: adjustParam(AdjustTarget::Balance, -1); break;
            case Action::GAIN_UP:      adjustParam(AdjustTarget::Gain, +1); break;
            case Action::GAIN_DOWN:    adjustParam(AdjustTarget::Gain, -1); break;
            case Action::ENC_PRESS:
                if (e.longPress) {
                    toggleUserMute();
                } else if (!e.repeat) {
                    cycleTarget();
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

// --- Періодика (ramp, таймери, синхронізація) -------------------------------
void tickLocked() {
    const uint32_t now = millis();

    ampService(now);  // [Prompt 23c]
    restartService(now);  // [Prompt 28] (ESP.restart() не повертається)

    if (s_otaAbortPending) {  // [Prompt 16] otaFailed() не отримав мʼютекс
        s_otaAbortPending = false;
        leaveOtaLocked();
    }

    // [Prompt 23c] Розмʼют лише коли підсилювач прогрітий (або не потрібен).
    if (s_phase == Phase::Settle && reached(now, s_phaseStartMs + cfg::kUnmuteDelayMs) &&
        ampReady()) {
        s_phase = Phase::Ramp;
        s_rampCur = s_caps.volumeMin;
        s_rampLastStepMs = now;
        applyHardwareMute();  // знімає мʼют переходу (userMute/Standby лишаються)
        APP_LOG("unmute, ramp %d -> %d\n", static_cast<int>(s_rampCur),
                static_cast<int>(s_volume));
    }

    if (s_phase == Phase::Ramp && reached(now, s_rampLastStepMs + cfg::kRampStepMs)) {
        s_rampLastStepMs = now;
        int next = static_cast<int>(s_rampCur) + cfg::kRampStep;
        if (next >= s_volume) {
            next = s_volume;
            s_phase = Phase::None;
            APP_LOG("ramp done vol=%d\n", next);
        }
        s_rampCur = static_cast<int16_t>(next);
        if (s_proc != nullptr) {
            s_proc->setVolume(static_cast<int8_t>(next));
        }
    }

    if (s_gainHoldActive && reached(now, s_gainHoldUntilMs)) {
        s_gainHoldActive = false;
        applyHardwareMute();
    }

    if (s_proc != nullptr && desiredMute() != s_hwMute &&
        static_cast<uint32_t>(now - s_lastMuteTryMs) >= cfg::kMuteRetryMs) {
        applyHardwareMute();
    }

    followWifiMode();  // [Prompt 12] ДОДАНО
    syncIrLearn(now);  // [Prompt 15] ДОДАНО

    // [Prompt 31] Автоповернення цілі регулювання на гучність після kAdjustTimeoutMs без
    // подій енкодера (працює в усіх режимах; тик іде й під час навчання IR / OTA).
    if (s_target != AdjustTarget::Volume && reached(now, s_lastEncMs + cfg::kAdjustTimeoutMs)) {
        s_target = AdjustTarget::Volume;
        APP_LOG("adjust target -> volume (timeout)\n");
        publishState();
    }

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
            (s_phase != Phase::None) || s_gainHoldActive || (s_mode == Mode::IrLearn) ||
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
    if (s_mode == Mode::Standby && !powerCmd) {
        why = "standby";  // як і події: у Standby все, крім POWER, ігнорується
        return WebCmdResult::NotAllowed;
    }

    switch (c.type) {
        case WebCmdType::VolumeSet:
            if (c.value < s_caps.volumeMin || c.value > s_caps.volumeMax) {
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
            if (s_proc == nullptr || !targetSupported(AdjustTarget::Gain)) {
                why = "not_supported";
                return WebCmdResult::Unsupported;
            }
            if (c.value < s_caps.gainMin || c.value > s_caps.gainMax) {
                why = "gain_out_of_range";
                return WebCmdResult::OutOfRange;
            }
            // adjustGain() сам мʼютить на час стрибка й нічого не робить при рівності.
            adjustGain(static_cast<int>(c.value) - static_cast<int>(s_gain));
            break;

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
            if (c.value < 0 || c.value >= s_caps.inputCount ||
                !inputAvailable(static_cast<uint8_t>(c.value))) {  // [Prompt 28]: офлайн -> без радіо
                why = "input_unavailable";  // напр. вхід 3 для PT2313L
                return WebCmdResult::OutOfRange;
            }
            if (s_mode == Mode::Menu) closeStationList();
            changeInput(static_cast<uint8_t>(c.value));
            break;

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
                stopStream();
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
    if (s_mode == Mode::OtaUpdate || restarting()) {  // [Prompt 16] під час прошивки чип не чіпаємо; [Prompt 28] і перезапуску
        xSemaphoreGiveRecursive(s_lock);
        return false;
    }
    if (s_proc != nullptr) {
        if (u.hasBass && s_caps.bass && s_proc->setBass(u.bass)) {
            s_bass = u.bass;
            u.bassOk = true;
            APP_LOG("bass=%d (WEB)\n", static_cast<int>(s_bass));
        }
        if (u.hasTreble && s_caps.treble && s_proc->setTreble(u.treble)) {
            s_treble = u.treble;
            u.trebleOk = true;
            APP_LOG("treble=%d (WEB)\n", static_cast<int>(s_treble));
        }
        if (u.hasBalance && s_caps.balance && s_proc->setBalance(u.balance)) {
            s_balance = u.balance;
            u.balanceOk = true;
            APP_LOG("balance=%d (WEB)\n", static_cast<int>(s_balance));
        }
        // [Prompt 21b] Тонкомпенсація теж через контролер: профіль поточного входу + AppState.
        if (u.hasLoudness && s_caps.loudness && s_proc->setLoudness(u.loudness)) {
            s_loudness = u.loudness;
            u.loudnessOk = true;
            APP_LOG("loudness=%d (WEB)\n", s_loudness ? 1 : 0);
        }
    }
    if (u.bassOk || u.trebleOk || u.balanceOk || u.loudnessOk) {
        publishState();
        persist();
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
        s_phase = Phase::None;  // Settle/Ramp скасовано
        s_gainHoldActive = false;
        applyHardwareMute();    // desiredMute() уже true; мʼют ПЕРЕД зупинкою потоку
        stopStream();
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

bool AppController::begin(AudioProcessor* processorOrNull) {
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

    s_proc = processorOrNull;
    loadCaps();
    if (s_proc == nullptr) {
        Serial.println("[APP] no audio processor: sound control unavailable");
    }

    // Налаштування — лише з Settings; значення обрізаємо за capabilities
    // (Settings діапазони не валідує).
    // [Prompt 21b] Профілі всіх входів — у таблицю контролера, робочі значення — з профілю
    // останнього активного входу.
    const Settings st = SettingsStore::snapshot();
    memcpy(s_profiles, st.profiles, sizeof(s_profiles));
    s_input = (st.lastInput < s_caps.inputCount) ? st.lastInput : 0;
    s_station = (st.lastStation < stationCount()) ? st.lastStation : 0;
    loadActiveFromProfile(s_input);
    s_userMute = st.lastMute;
    s_target = AdjustTarget::Volume;
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    s_phase = Phase::None;
    s_gainHoldActive = false;
    s_mode = Mode::Standby;  // до завершення старту — тиша (desiredMute() == true)

    // Спершу тиша, потім решта.
    s_hwMute = false;
    applyHardwareMute(true);

    if (cfg::kBootInStandby) {
        APP_LOG("boot in standby\n");
        // Вхід у чіп не перемикаємо (як і раніше), тож gain (він стосується поточного входу
        // ЧІПА) не чіпаємо: профіль входу застосується повністю при виході зі standby.
        applyActiveToChip(false);
    } else {
        powerOnTransition();  // режим, setInput + профіль, playUrl для Radio, ramp у tick()
    }
    publishState();

    BaseType_t ok = xTaskCreatePinnedToCore(taskMain, "app_ctrl", cfg::kTaskStackBytes, nullptr,
                                            cfg::kTaskPriority, nullptr, cfg::kTaskCore);
    if (ok != pdPASS) {
        Serial.println("[APP] task create failed");
        return false;
    }

    s_started = true;
    APP_LOG("ready: mode=%s input=%u station=%u vol=%d mute=%d\n", modeName(s_mode),
            static_cast<unsigned>(s_input), static_cast<unsigned>(s_station),
            static_cast<int>(s_volume), s_userMute ? 1 : 0);
    return true;
}