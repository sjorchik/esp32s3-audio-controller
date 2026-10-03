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

#include "core/app_controller.h"

#include <Arduino.h>
#include <string.h>

#include "audio/audio_player.h"
#include "audio/audio_processor.h"
#include "config/app_controller_config.h"
#include "config/defaults.h"
#include "core/app_state.h"
#include "core/settings.h"
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
bool s_userMute = false;
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

// --- Мʼют атенюаторів ------------------------------------------------------
bool desiredMute() {
    return s_userMute || s_mode == Mode::Standby || s_phase == Phase::Settle ||
           s_gainHoldActive;
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
}

void publishState() {
    PubCtx p = {s_mode,   s_input, s_volume, s_bass,    s_treble, s_balance,
                s_gain,   s_userMute, s_station, s_target, s_menuCtx, s_menuSel};
    AppState::modify(applyPub, &p);
}

struct PersistCtx {
    uint8_t input;
    uint16_t station;
    int8_t volume, bass, treble, balance;
    bool mute;
    bool saveBass, saveTreble, saveBalance;
};

void applyPersist(Settings& s, void* c) {
    const PersistCtx* p = static_cast<const PersistCtx*>(c);
    s.lastInput = p->input;
    s.lastStation = p->station;
    s.lastVolume = p->volume;
    s.lastMute = p->mute;
    // Тембр/баланс не затираємо, якщо поточний чип їх не підтримує.
    if (p->saveBass) s.bass = p->bass;
    if (p->saveTreble) s.treble = p->treble;
    if (p->saveBalance) s.balance = p->balance;
}

// SettingsStore::modify() сама позначає кеш «брудним» (як requestSave()):
// фактичний запис відбудеться з дебаунсом.
void persist() {
    PersistCtx p = {s_input,   s_station,     s_volume,       s_bass,        s_treble,
                    s_balance, s_userMute,    s_caps.bass,    s_caps.treble, s_caps.balance};
    SettingsStore::modify(applyPersist, &p);
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
}

void syncPlayer() {
    SyncCtx c = {};
    if (s_input == 0 && s_mode != Mode::Standby) {
        AudioPlayer::currentMetadata(c.station, sizeof(c.station), c.title, sizeof(c.title));
        if (c.station[0] == '\0') {
            stationLabel(s_station, c.station, sizeof(c.station));
        }
    }
    c.playing = AudioPlayer::isPlaying();
    c.wifi = wifiUp();
    // [Prompt 12] ДОДАНО
    WifiManager::copyInfo(c.wifiSsid, sizeof(c.wifiSsid), c.wifiIp, sizeof(c.wifiIp));
    c.wifiApMode = WifiManager::isApMode();

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

void refreshGainFromChip() {
    if (s_proc != nullptr) {
        s_gain = s_proc->cachedState().gain;
    }
}

// Увімкнення: режим за s_input, перехід, вхід у чіпі, запуск потоку для Radio.
// Вхід/станцію викликач уже поклав у s_input/s_station.
void powerOnTransition() {
    s_mode = (s_input == 0) ? Mode::Radio : Mode::ExternalInput;
    s_menuCtx = MenuContext::None;
    s_menuSel = 0;
    startTransition();
    if (s_proc != nullptr) {
        s_proc->setInput(s_input);
        refreshGainFromChip();
    }
    if (s_input == 0) {
        startStream();
    }
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

    s_input = newIdx;
    s_mode = (newIdx == 0) ? Mode::Radio : Mode::ExternalInput;
    startTransition();  // мʼют -> гучність у мінімум
    if (leavingRadio) {
        stopStream();
    }
    if (s_proc != nullptr) {
        s_proc->setInput(newIdx);
        refreshGainFromChip();  // gain нового входу підставив драйвер
    }
    if (enteringRadio) {
        startStream();
    }
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
    s_persistNeeded = true;
}

void stepStation(int dir) {
    const int count = stationCount();
    if (count < 2) {
        return;
    }
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
    const int cur = s_proc->cachedState().gain;
    const int nv = clampInt(cur + d, s_caps.gainMin, s_caps.gainMax);
    if (nv == cur) {
        return;
    }
    s_gainHoldActive = true;
    s_gainHoldUntilMs = millis() + cfg::kGainMuteHoldMs;
    applyHardwareMute();
    s_proc->setGain(static_cast<int8_t>(nv));
    refreshGainFromChip();
    APP_LOG("gain=%d (mute hold)\n", static_cast<int>(s_gain));
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

void handleLocked(const Event& e) {
#if APP_CONTROLLER_LOG_EVENTS && APP_CONTROLLER_DEBUG
    Serial.printf("[APP] evt %s %s%s%s delta=%d\n", sourceName(e.source), actionName(e.action),
                  e.repeat ? " repeat" : "", e.longPress ? " long" : "",
                  static_cast<int>(e.delta));
#endif

    if (e.action == Action::POWER) {
        if (!e.longPress && !e.repeat) {
            togglePower();
        } else {
            APP_LOG("POWER long/repeat ignored\n");
        }
    } else if (s_mode == Mode::Standby || s_mode == Mode::IrLearn) {
        // Standby: усе, крім POWER, ігнорується. IrLearn поки ніхто не вмикає.
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

    if (s_playPending && s_input == 0 &&
        (s_mode == Mode::Radio || s_mode == Mode::Menu) && wifiUp()) {
        APP_LOG("Wi-Fi up: starting deferred play\n");
        startStream();
    }

    if (static_cast<uint32_t>(now - s_lastSyncMs) >= cfg::kSyncPeriodMs) {
        s_lastSyncMs = now;
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
        const bool active = (s_phase != Phase::None) || s_gainHoldActive;
        Event ev;
        const TickType_t timeout =
            pdMS_TO_TICKS(active ? cfg::kActivePollMs : cfg::kIdlePollMs);
        if (EventBus::poll(ev, timeout)) {
            AppController::handleEvent(ev);
        }
        tick();
    }
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
    u.bassOk = u.trebleOk = u.balanceOk = false;
    if (!s_started || s_lock == nullptr ||
        xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(cfg::kLockTimeoutMs)) != pdTRUE) {
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
    }
    if (u.bassOk || u.trebleOk || u.balanceOk) {
        publishState();
        persist();
    }
    xSemaphoreGiveRecursive(s_lock);
    return true;
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
    const Settings st = SettingsStore::snapshot();
    s_input = (st.lastInput < s_caps.inputCount) ? st.lastInput : 0;
    s_station = (st.lastStation < stationCount()) ? st.lastStation : 0;
    s_volume = static_cast<int8_t>(clampInt(st.lastVolume, s_caps.volumeMin, s_caps.volumeMax));
    s_bass = s_caps.bass
                 ? static_cast<int8_t>(clampInt(st.bass, s_caps.toneMin, s_caps.toneMax))
                 : 0;
    s_treble = s_caps.treble
                   ? static_cast<int8_t>(clampInt(st.treble, s_caps.toneMin, s_caps.toneMax))
                   : 0;
    s_balance = s_caps.balance ? static_cast<int8_t>(clampInt(st.balance, s_caps.balanceMin,
                                                              s_caps.balanceMax))
                               : 0;
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
    if (s_proc != nullptr) {
        if (s_caps.bass) s_proc->setBass(s_bass);
        if (s_caps.treble) s_proc->setTreble(s_treble);
        if (s_caps.balance) s_proc->setBalance(s_balance);
        if (s_caps.loudness) s_proc->setLoudness(st.loudness);
    }

    if (cfg::kBootInStandby) {
        APP_LOG("boot in standby\n");
    } else {
        powerOnTransition();  // режим, setInput, playUrl для Radio, ramp у tick()
    }
    refreshGainFromChip();
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