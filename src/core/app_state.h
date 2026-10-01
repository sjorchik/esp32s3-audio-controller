#pragma once

// Єдине сховище стану пристрою.
// Доступ потокобезпечний (мʼютекс всередині).
// [Prompt 6] Реалізовано core/app_state.cpp; ДОДАНО modify() (два варіанти).
// begin() викличе AppController (Prompt 7); до того snapshot()/modify() працюють
// без блокування (однопотокова рання фаза старту).

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

// Дані стану.
// Розміри буферів під метадані залишаються константами.
struct AppStateData {
    Mode mode;
    uint8_t inputIndex;

    int8_t volume;
    int8_t bass;
    int8_t treble;
    int8_t balance;
    bool mute;

    uint16_t stationIndex;
    char stationName[64];
    char trackTitle[128];

    bool wifiConnected;
    bool streamPlaying;

    // Рівні VU, нормалізовані 0..1.
    float vuLeft;
    float vuRight;
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
