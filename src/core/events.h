#pragma once

// Подієва шина.
// Єдина FreeRTOS-черга подій.
// Джерела: кнопки, енкодер, IR, веб.
// Модулі не викликають один одного напряму для керування.

#include <Arduino.h>
#include <stdint.h>

// Логічні дії.
// Нові значення додавати ЛИШЕ в кінець: числові значення Action зберігаються в NVS (мапа IR).
enum class Action : uint8_t {
    POWER,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    OK,
    ENC_CW,
    ENC_CCW,
    ENC_PRESS,
    // [Prompt 2] ДОДАНО: VOL_UP..BACK, INPUT_RADIO..INPUT_AUX, DIGIT_0..DIGIT_9 (цифри йдуть підряд).
    VOL_UP,
    VOL_DOWN,
    MUTE,
    MENU,
    BACK,
    INPUT_RADIO,
    INPUT_TV,
    INPUT_PC,
    INPUT_AUX,
    DIGIT_0,
    DIGIT_1,
    DIGIT_2,
    DIGIT_3,
    DIGIT_4,
    DIGIT_5,
    DIGIT_6,
    DIGIT_7,
    DIGIT_8,
    DIGIT_9,
    // [Prompt 7] ДОДАНО (лише в кінець — порядок наявних значень не змінено).
    BASS_UP,
    BASS_DOWN,
    TREBLE_UP,
    TREBLE_DOWN,
    BALANCE_UP,
    BALANCE_DOWN,
    GAIN_UP,
    GAIN_DOWN,
};

// Джерело події.
enum class EventSource : uint8_t {
    BUTTON,
    ENCODER,
    IR,
    WEB,
};

// Подія.
// Прапорець repeat означає утримання.
struct Event {
    Action action;
    EventSource source;
    bool repeat;

    // Довге натискання: одна подія в момент досягнення порога утримання.
    // Для repeat-подій і коротких натискань завжди false.
    bool longPress = false;

    // Кількість кроків енкодера (з урахуванням акселерації).
    // Завжди >= 0: напрямок задає Action (ENC_CW / ENC_CCW).
    // Для подій, не повʼязаних з обертанням енкодера, 0.
    int8_t delta = 0;

    // [Prompt 28] ДОДАНО (лише в кінець): дуже довге утримання — одна подія в момент
    // досягнення input_cfg::kPowerRestartHoldMs (зараз лише кнопка POWER). Така подія
    // завжди має й longPress == true, тож споживач, який не знає про це поле, трактує її
    // як звичайне довге натискання (тобто ігнорує, як і раніше). Для решти подій false.
    bool veryLongPress = false;
};

// Мінімальна підготовка шини подій.
// Реалізація логіки обробки буде в AppController.
class EventBus {
public:
    // Створює чергу подій.
    static bool begin();

    // Надіслати подію.
    // Для виклику з ISR використовувати пост-варіант без блокування.
    static bool post(const Event& event, TickType_t timeout = 0);

    // Отримати подію.
    // Використовується задачею обробки.
    static bool poll(Event& event, TickType_t timeout = portMAX_DELAY);

    // Чи шина ініціалізована.
    static bool isReady();
};