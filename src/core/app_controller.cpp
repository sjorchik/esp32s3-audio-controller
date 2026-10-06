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

#include "core/app_controller.h"

#include <Arduino.h>
#include <string.h>

#include "audio/audio_player.h"
#include "audio/audio_processor.h"
#include "config/app_controller_config.h"
#include "config/defaults.h"
#include "config/ir_learn_ui_config.h"  // [Prompt 15] ДОДАНО
#include "core/app_state.h"
#include "core/settings.h"
#include "input/ir_rc5.h"            // [Prompt 15] ДОДАНО
#include "net/wifi_manager.h"       // [Prompt 12] ДОДАНО
#include "stations/station_store.h"  // [Prompt 11] ДОДАНО

namespace cfg = app_controller_cfg;

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
bool s_persistNeeded = false;

// --- [Prompt 15] Навчання IR ---
Mode s_irPrevMode = Mode::Radio;  // режим ДО навчання; осмислений лише при s_mode == IrLearn
bool s_irHolding = false;         // фінальний результат (Success/Timeout) вже показується
uint32_t s_irHoldUntilMs = 0;
// Остання опублікована в AppState трійка (щоб не смикати мʼютекс стану щотакту).
IrLearnStatus s_irPubStatus = IrLearnStatus::Idle;
Action s_irPubTarget = Action::POWER;
Action s_irPubOther = Action::POWER;

// --- [Prompt 16] OTA ---
Mode s_otaPrevMode = Mode::Radio;         // режим ДО OTA; осмислений лише при s_mode == OtaUpdate
volatile uint8_t s_otaPubPercent = 0;     // остання опублікована цифра (читає setOtaProgress без мʼютекса)
volatile bool s_otaAbortPending = false;  // otaFailed() не взяв мʼютекс -> tickLocked() відновить

// ---------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------
inline bool reached(uint32_t now, uint32_t t) {
    return static_cast<int32_t>(now - t) >= 0;
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

void startStream() {
    if (stationCount() == 0) {
        s_playPending = false;
        APP_LOG("no stations: nothing to play\n");
        return;
    }
    clampStationIndex();  // [Prompt 14] список міг скоротитись з вебу
    if (wifiUp()) {
        s_playPending = false;
        Station st;
        if (!StationStore::get(s_station, st)) {
            APP_LOG("station %u not found\n", static_cast<unsigned>(s_station));
            return;
        }
        if (!AudioPlayer::playUrl(st.url)) {
            APP_LOG("playUrl rejected\n");
        }
    } else {
        s_playPending = true;
        APP_LOG("no Wi-Fi: play deferred\n");
    }
}

void stopStream() {
    s_playPending = false;
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
           s_mode == Mode::OtaUpdate;  // [Prompt 16]: під час прошивки тиша
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
    memcpy(s.inputName, p->inputName, sizeof(s.inputName));  // [Prompt 23b]
}

void publishState() {
    PubCtx p = {s_mode,   s_input, s_volume, s_bass,    s_treble, s_balance,
                s_gain,   s_userMute, s_station, s_target, s_menuCtx, s_menuSel,
                s_loudness};
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
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    startTransition();
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
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    s_mode = Mode::Standby;
    applyHardwareMute();  // Standby входить у desiredMute(): негайно, без ramp
    stopStream();
    persist();
    SettingsStore::flush();
    s_persistNeeded = false;
}

void leaveStandby() {
    captureActiveProfile();  // [Prompt 21b] профіль «старого» входу, поки s_input ще його
    const Settings st = SettingsStore::snapshot();
    s_input = (st.lastInput < s_caps.inputCount) ? st.lastInput : 0;
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
    const int n = (static_cast<int>(s_input) + dir + count) % count;
    changeInput(static_cast<uint8_t>(n));
}

void selectInput(uint8_t idx) {
    if (idx >= s_caps.inputCount) {
        APP_LOG("input %u unavailable on this processor\n", static_cast<unsigned>(idx));
        return;
    }
    changeInput(idx);
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
    if (s_mode == Mode::OtaUpdate) {
        return false;  // [Prompt 16]
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

void handleLocked(const Event& e) {
#if APP_CONTROLLER_LOG_EVENTS && APP_CONTROLLER_DEBUG
    Serial.printf("[APP] evt %s %s%s%s delta=%d\n", sourceName(e.source), actionName(e.action),
                  e.repeat ? " repeat" : "", e.longPress ? " long" : "",
                  static_cast<int>(e.delta));
#endif

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
            APP_LOG("POWER long/repeat ignored\n");
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
    const bool ap = WifiManager::isApMode();
    Mode want = s_mode;
    if (ap && s_mode == Mode::Radio) {
        want = Mode::WifiSetup;
    } else if (!ap && s_mode == Mode::WifiSetup) {
        want = Mode::Radio;
    }
    if (want == s_mode) {
        return;
    }
    s_mode = want;
    // WifiSetup: відкладений старт потоку не потрібен (AP завершується перезапуском).
    // Назад у Radio: дозволити відкладений старт, коли зʼявиться Wi-Fi.
    s_playPending = (want == Mode::Radio);
    APP_LOG("mode -> %s (Wi-Fi)\n", modeName(s_mode));
    publishState();
}

// --- Періодика (ramp, таймери, синхронізація) -------------------------------
void tickLocked() {
    const uint32_t now = millis();

    if (s_otaAbortPending) {  // [Prompt 16] otaFailed() не отримав мʼютекс
        s_otaAbortPending = false;
        leaveOtaLocked();
    }

    if (s_phase == Phase::Settle && reached(now, s_phaseStartMs + cfg::kUnmuteDelayMs)) {
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

    if (s_playPending && s_input == 0 &&
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
            (s_phase != Phase::None) || s_gainHoldActive || (s_mode == Mode::IrLearn);
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
            if (c.value < 0 || c.value >= s_caps.inputCount) {
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
    if (s_mode == Mode::OtaUpdate) {  // [Prompt 16] під час прошивки чип не чіпаємо
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
    if (s_mode != Mode::IrLearn && s_mode != Mode::OtaUpdate) {
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