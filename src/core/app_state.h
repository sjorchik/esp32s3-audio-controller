#pragma once

// Єдине сховище стану пристрою.
// Доступ потокобезпечний (мʼютекс всередині).
// [Prompt 6] Реалізовано core/app_state.cpp; ДОДАНО modify() (два варіанти).
// [Prompt 8] begin() викликає AppController (першим ділом у своєму begin()).
// [Prompt 8] ДОДАНО: enum AdjustTarget, enum MenuContext та чотири поля В КІНЕЦЬ
// AppStateData (gain, adjustTarget, menuContext, menuSelection). Нові поля
// додано лише в кінець структури, тому код, що ініціалізує наявні поля за
// іменами чи агрегатно, лишається чинним (нові поля нульові).
// [Prompt 10] ДОДАНО: enum StreamStatus та поле streamStatus для детального
// статусу потоку у UI. Незалежно від audio/audio_player.h.
// [Prompt 12] ДОДАНО: wifiSsid, wifiIp, wifiApMode (лише в кінець структури) —
// дані net/wifi_manager для екрана Mode::WifiSetup. wifiConnected (Prompt 6)
// тепер заповнюється з WifiManager::isConnected().

#include <Arduino.h>
#include <stdint.h>

// Режими пристрою.
enum class Mode : uint8_t {
    Standby,
    Radio,
    ExternalInput,
    Menu,
    IrLearn,
    WifiSetup,
};

// [Prompt 8] ДОДАНО: яким параметром зараз керує обертання енкодера.
// Порядок = порядок циклу кліку енкодера (Volume -> Bass -> Treble -> Balance -> Gain).
enum class AdjustTarget : uint8_t {
    Volume,
    Bass,
    Treble,
    Balance,
    Gain,
};

// [Prompt 8] ДОДАНО: який саме підрежим активний, коли mode == Mode::Menu.
// Нові підрежими (меню налаштувань тощо) додавати лише в кінець.
enum class MenuContext : uint8_t {
    None,
    StationList,
};

// [Prompt 10] ДОДАНО: детальний статус потоку для UI. Незалежно від
// audio_player.h::PlayerState, але семантично відповідає йому.
// Дозволяє ui/screens розрізнити Buffering/Error/Reconnecting без прямого
// доступу до AudioPlayer.
enum class StreamStatus : uint8_t {
    Idle,         // нічого не відтворюється (початковий стан)
    Connecting,   // перша спроба підключення до станції
    Buffering,    // наповнюється буфер або просів під час відтворення
    Playing,      // потік грає
    Error,        // потік обірвався/не підключився
    Reconnecting, // повторна спроба підключення після Error
};

// Дані стану.
// Розміри буферів під метадані залишаються константами.
struct AppStateData {
    Mode mode;
    uint8_t inputIndex;

    int8_t volume;   // ЦІЛЬОВА гучність (під час ramp в чіпі може бути менше)
    int8_t bass;
    int8_t treble;
    int8_t balance;
    bool mute;       // мʼют, який зробив користувач (Standby/переходи сюди не входять)

    uint16_t stationIndex;
    char stationName[64];
    char trackTitle[128];

    bool wifiConnected;
    bool streamPlaying;

    // Рівні VU, нормалізовані 0..1.
    float vuLeft;
    float vuRight;

    // --- [Prompt 8] ДОДАНО (лише в кінець) ---
    // Підсилення ПОТОЧНОГО входу, сирі апаратні кроки (0..3 для обох чипів).
    int8_t gain;
    // Ціль, яку регулює обертання енкодера.
    AdjustTarget adjustTarget;
    // Підрежим меню; осмислений лише при mode == Mode::Menu (інакше None).
    MenuContext menuContext;
    // Вибраний пункт у меню/списку (для StationList — індекс станції).
    uint16_t menuSelection;

    // --- [Prompt 10] ДОДАНО ---
    // Детальний статус потоку для UI (Idle/Connecting/Buffering/Playing/Error/Reconnecting).
    StreamStatus streamStatus;

    // --- [Prompt 12] ДОДАНО ---
    // Поточна Wi-Fi мережа (STA) або назва точки доступу (AP); "" якщо невідомо.
    char wifiSsid[33];
    // IP: отриманий (STA) або адреса AP ("192.168.4.1"); "" якщо немає.
    char wifiIp[16];
    // true: пристрій у режимі AP (captive portal), STA не підключена.
    bool wifiApMode;
};

class AppState {
public:
    // Створює мʼютекс і заповнює стан значеннями з config/defaults.h
    // (mode = Standby). Повторні виклики нічого не скидають.
    static bool begin();

    // Потокобезпечна копія стану.
    static AppStateData snapshot();

    // Потокобезпечна ПОВНА заміна стану. Залишено для сумісності.
    // Патерн «snapshot() → змінити поле → update()» НЕ атомарний: паралельна
    // зміна іншого поля іншою задачею (наприклад, VU з audio-ядра) буде
    // затерта. Для зміни окремих полів користуйтеся modify().
    static void update(const AppStateData& data);

    // [Prompt 6] ДОДАНО: атомарна часткова зміна. fn викликається під мʼютексом
    // над внутрішнім станом і міняє лише потрібні поля. fn має бути короткою й
    // не викликати AppState / Serial / інші блокуючі функції.
    // fn — звичайна функція або лямбда без захоплень (вказівник не має контексту;
    // для передачі значення див. перевантаження нижче).
    static void modify(void (*fn)(AppStateData&));

    // [Prompt 6] ДОДАНО: те саме з довільним контекстом, напр.
    //   int8_t v = 30;
    //   AppState::modify([](AppStateData& s, void* c) { s.volume = *static_cast<int8_t*>(c); }, &v);
    static void modify(void (*fn)(AppStateData&, void* ctx), void* ctx);
};