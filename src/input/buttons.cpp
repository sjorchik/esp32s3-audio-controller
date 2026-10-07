#include "input/buttons.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

#include <atomic>

#include "config/input_config.h"
#include "config/pins.h"
#include "core/events.h"

namespace cfg = input_cfg;

namespace {

// Опис однієї кнопки: пін, логічна дія, джерело, імʼя для логу та режими.
struct ButtonDef {
    uint8_t pin;
    Action action;
    EventSource source;
    const char* name;
    bool longPressEnabled;  // довге натискання (прапорець з input_config.h)
    bool repeatEnabled;     // автоповтор при утриманні (лише стрілки)
    uint32_t veryLongMs;    // [Prompt 28] дуже довге утримання, мс (0 = немає; лише POWER)
};

constexpr ButtonDef kButtons[] = {
    {pins::kBtnPower, Action::POWER,     EventSource::BUTTON,  "POWER",     cfg::kBtnLongPressPower,  false, cfg::kPowerRestartHoldMs},
    {pins::kBtnUp,    Action::UP,        EventSource::BUTTON,  "UP",        cfg::kBtnLongPressUp,     true,  0},
    {pins::kBtnDown,  Action::DOWN,      EventSource::BUTTON,  "DOWN",      cfg::kBtnLongPressDown,   true,  0},
    {pins::kBtnLeft,  Action::LEFT,      EventSource::BUTTON,  "LEFT",      cfg::kBtnLongPressLeft,   true,  0},
    {pins::kBtnRight, Action::RIGHT,     EventSource::BUTTON,  "RIGHT",     cfg::kBtnLongPressRight,  true,  0},
    {pins::kBtnOk,    Action::OK,        EventSource::BUTTON,  "OK",        cfg::kBtnLongPressOk,     false, 0},
    {pins::kEncBtn,   Action::ENC_PRESS, EventSource::ENCODER, "ENC_PRESS", cfg::kBtnLongPressEncBtn, false, 0},
};

constexpr size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

// Стан однієї кнопки в задачі опитування.
struct ButtonState {
    bool raw = false;                 // останній сирий (недебаунсований) стан
    bool debounced = false;           // стабільний стан: true = натиснуто
    uint32_t rawSinceMs = 0;          // коли сирий стан востаннє змінився
    uint32_t pressedAtMs = 0;         // коли натискання було прийнято
    uint32_t lastRepeatMs = 0;        // коли відправлено останній повтор
    bool longSent = false;            // довге вже відправлено в цьому натисканні
    bool veryLongSent = false;        // [Prompt 28] дуже довге вже відправлено в цьому натисканні
    bool repeating = false;           // почалися повтори в цьому натисканні
    bool ignoreUntilRelease = false;  // кнопка була натиснута при старті задачі
};

enum class Kind : uint8_t { Short, Repeat, Long, VeryLong };  // [Prompt 28] +VeryLong

TaskHandle_t s_task = nullptr;
std::atomic<uint32_t> s_dropped{0};

bool readPressed(uint8_t pin) {
    return digitalRead(pin) == cfg::kBtnPressedLevel;
}

// Формує подію, логує (INPUT_DEBUG) і кладе в чергу без очікування.
void emit(const ButtonDef& def, Kind kind) {
    const bool isRepeat = (kind == Kind::Repeat);
    const bool isVeryLong = (kind == Kind::VeryLong);
    // [Prompt 28] Дуже довге — це теж довге (longPress = true): старі споживачі його ігнорують.
    const bool isLong = (kind == Kind::Long) || isVeryLong;

#if INPUT_DEBUG
    Serial.printf("[BTN] %s %s\n", def.name,
                  isVeryLong ? "very-long" : (isLong ? "long" : (isRepeat ? "repeat" : "short")));
#endif

    const Event ev{def.action, def.source, isRepeat, isLong, 0, isVeryLong};
    if (!EventBus::post(ev, 0)) {
        s_dropped.fetch_add(1);
#if INPUT_DEBUG
        Serial.printf("[BTN] %s dropped (queue full)\n", def.name);
#endif
    }
}

// Один крок опитування для однієї кнопки.
void processButton(const ButtonDef& def, ButtonState& st, bool rawPressed, uint32_t now) {
    // 1. Дебаунс за часом: новий сирий стан приймається після kBtnDebounceMs стабільності.
    if (rawPressed != st.raw) {
        st.raw = rawPressed;
        st.rawSinceMs = now;
    }
    if (st.raw != st.debounced && (now - st.rawSinceMs) >= cfg::kBtnDebounceMs) {
        st.debounced = st.raw;
        if (st.debounced) {
            // Натискання прийнято: починаємо відлік утримання.
            st.pressedAtMs = now;
            st.longSent = false;
            st.veryLongSent = false;  // [Prompt 28]
            st.repeating = false;
        } else {
            // Відпускання: коротка подія лише якщо не було ні довгого, ні повторів.
            if (st.ignoreUntilRelease) {
                st.ignoreUntilRelease = false;
            } else if (!st.longSent && !st.repeating) {
                emit(def, Kind::Short);
            }
            return;
        }
    }

    // 2. Утримання: повтори або довге натискання.
    if (!st.debounced || st.ignoreUntilRelease) {
        return;
    }
    const uint32_t held = now - st.pressedAtMs;

    if (st.repeating) {
        if ((now - st.lastRepeatMs) >= cfg::kBtnRepeatPeriodMs) {
            st.lastRepeatMs = now;
            emit(def, Kind::Repeat);
        }
    } else if (!st.longSent) {
        // Що настане раніше, те й блокує друге в межах цього натискання.
        if (def.repeatEnabled && held >= cfg::kBtnRepeatDelayMs) {
            st.repeating = true;
            st.lastRepeatMs = now;
            emit(def, Kind::Repeat);
        } else if (def.longPressEnabled && held >= cfg::kBtnLongPressMs) {
            st.longSent = true;
            emit(def, Kind::Long);
        }
    } else if (def.veryLongMs != 0 && !st.veryLongSent && held >= def.veryLongMs) {
        // [Prompt 28] Довге вже було (longSent) — тепер дуже довге, один раз за натискання.
        st.veryLongSent = true;
        emit(def, Kind::VeryLong);
    }
}

void buttonsTask(void* /*arg*/) {
    // Даємо підтяжкам встановити рівні, потім фіксуємо початковий стан.
    vTaskDelay(pdMS_TO_TICKS(cfg::kBtnPullupSettleMs));

    ButtonState states[kButtonCount] = {};
    const uint32_t start = millis();
    for (size_t i = 0; i < kButtonCount; ++i) {
        const bool pressed = readPressed(kButtons[i].pin);
        states[i].raw = pressed;
        states[i].debounced = pressed;
        states[i].rawSinceMs = start;
        states[i].pressedAtMs = start;
        // Кнопка, утримувана від старту (наприклад OK для скидання Wi-Fi),
        // не генерує подій, доки її не відпустять.
        states[i].ignoreUntilRelease = pressed;
    }

    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(cfg::kBtnPollMs));
        const uint32_t now = millis();
        for (size_t i = 0; i < kButtonCount; ++i) {
            processButton(kButtons[i], states[i], readPressed(kButtons[i].pin), now);
        }
    }
}

}  // namespace

bool Buttons::begin() {
    if (s_task != nullptr) {
        return true;  // вже запущено
    }
    if (!EventBus::isReady()) {
        Serial.println("[BTN] begin failed: EventBus not ready");
        return false;
    }

    for (size_t i = 0; i < kButtonCount; ++i) {
        pinMode(kButtons[i].pin, INPUT_PULLUP);
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        buttonsTask, "buttons", cfg::kBtnTaskStackBytes, nullptr,
        cfg::kBtnTaskPriority, &s_task, cfg::kBtnTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        Serial.println("[BTN] begin failed: task create");
        return false;
    }
    return true;
}

uint32_t Buttons::droppedEvents() {
    return s_dropped.load();
}

bool Buttons::isHeldAtBoot(uint8_t pin, uint32_t ms) {
    pinMode(pin, INPUT_PULLUP);
    vTaskDelay(pdMS_TO_TICKS(cfg::kBtnPullupSettleMs));

    const uint32_t start = millis();
    for (;;) {
        if (digitalRead(pin) != cfg::kBtnPressedLevel) {
            return false;  // відпущено (або ще не натиснуто) — утримання перервано
        }
        if ((millis() - start) >= ms) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(cfg::kBootSamplePeriodMs));
    }
}
