#pragma once

// Єдине сховище стану пристрою.
// Доступ має бути потокобезпечним.
// Реалізація буде додана пізніше.

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
    // Ініціалізація сховища.
    static bool begin();

    // Потокобезпечна копія стану.
    static AppStateData snapshot();

    // Потокобезпечне оновлення стану.
    static void update(const AppStateData& data);
};