#include "input/encoder.h"

#include <Arduino.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <stdlib.h>

#include <atomic>

#include "config/input_config.h"
#include "config/pins.h"
#include "core/events.h"
#include "driver/pulse_cnt.h"

namespace cfg = input_cfg;

namespace {

pcnt_unit_handle_t s_unit = nullptr;
pcnt_channel_handle_t s_chanA = nullptr;
pcnt_channel_handle_t s_chanB = nullptr;
TaskHandle_t s_task = nullptr;
std::atomic<uint32_t> s_dropped{0};

bool check(esp_err_t err, const char* what) {
    if (err == ESP_OK) {
        return true;
    }
    Serial.printf("[ENC] %s failed: %s\n", what, esp_err_to_name(err));
    return false;
}

// Звільняє все, що встигли створити (безпечно викликати на частково
// ініціалізованому стані). Повернені коди навмисно ігноруються.
void destroyPcnt() {
    if (s_unit != nullptr) {
        pcnt_unit_stop(s_unit);
        pcnt_unit_disable(s_unit);
    }
    if (s_chanA != nullptr) {
        pcnt_del_channel(s_chanA);
        s_chanA = nullptr;
    }
    if (s_chanB != nullptr) {
        pcnt_del_channel(s_chanB);
        s_chanB = nullptr;
    }
    if (s_unit != nullptr) {
        pcnt_del_unit(s_unit);
        s_unit = nullptr;
    }
}

// Квадратурне декодування x4 на двох каналах одного юніта PCNT:
// канал A рахує за фронтами A з рівнем B як керуючим, канал B — навпаки.
// Послідовність "A випереджає B" дає +4 за повний цикл, зворотна — -4.
bool createPcnt() {
    // Внутрішній акумулятор (accum_count) + watch points на межах: тоді
    // pcnt_unit_get_count() повертає лічильник без втрат на переповненні.
    pcnt_unit_config_t unitCfg = {};
    unitCfg.low_limit = cfg::kEncPcntLowLimit;
    unitCfg.high_limit = cfg::kEncPcntHighLimit;
    unitCfg.flags.accum_count = 1;
    if (!check(pcnt_new_unit(&unitCfg, &s_unit), "new_unit")) {
        return false;
    }

    if (cfg::kEncGlitchNs > 0) {
        pcnt_glitch_filter_config_t filterCfg = {};
        filterCfg.max_glitch_ns = cfg::kEncGlitchNs;
        if (!check(pcnt_unit_set_glitch_filter(s_unit, &filterCfg), "glitch_filter")) {
            return false;
        }
    }

    // Драйвер PCNT сам вмикає внутрішню підтяжку на пінах каналів.
    pcnt_chan_config_t chanACfg = {};
    chanACfg.edge_gpio_num = pins::kEncA;
    chanACfg.level_gpio_num = pins::kEncB;
    if (!check(pcnt_new_channel(s_unit, &chanACfg, &s_chanA), "new_channel A")) {
        return false;
    }

    pcnt_chan_config_t chanBCfg = {};
    chanBCfg.edge_gpio_num = pins::kEncB;
    chanBCfg.level_gpio_num = pins::kEncA;
    if (!check(pcnt_new_channel(s_unit, &chanBCfg, &s_chanB), "new_channel B")) {
        return false;
    }

    return check(pcnt_channel_set_edge_action(s_chanA,
                     PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE),
                 "edge_action A") &&
           check(pcnt_channel_set_level_action(s_chanA,
                     PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
                 "level_action A") &&
           check(pcnt_channel_set_edge_action(s_chanB,
                     PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE),
                 "edge_action B") &&
           check(pcnt_channel_set_level_action(s_chanB,
                     PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
                 "level_action B") &&
           check(pcnt_unit_add_watch_point(s_unit, cfg::kEncPcntHighLimit), "watch_point high") &&
           check(pcnt_unit_add_watch_point(s_unit, cfg::kEncPcntLowLimit), "watch_point low") &&
           check(pcnt_unit_enable(s_unit), "enable") &&
           check(pcnt_unit_clear_count(s_unit), "clear_count") &&
           check(pcnt_unit_start(s_unit), "start");
}

// Формує подію, логує (INPUT_DEBUG) і кладе в чергу без очікування.
void emit(bool cw, int8_t delta) {
#if INPUT_DEBUG
    Serial.printf("[ENC] %s delta=%d\n", cw ? "CW" : "CCW", static_cast<int>(delta));
#endif

    const Event ev{cw ? Action::ENC_CW : Action::ENC_CCW, EventSource::ENCODER, false, false, delta};
    if (!EventBus::post(ev, 0)) {
        s_dropped.fetch_add(1);
#if INPUT_DEBUG
        Serial.println("[ENC] event dropped (queue full)");
#endif
    }
}

void encoderTask(void* /*arg*/) {
    // Лічильник очищено перед стартом PCNT, тому базова точка = 0
    // і імпульси між begin() та стартом задачі не губляться.
    int lastCount = 0;
    int remainder = 0;      // імпульси, що ще не склали цілий detent
    int8_t lastDir = 0;     // напрямок попередньої події: +1 / -1 / 0
    uint32_t lastEventMs = 0;

    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(cfg::kEncPollMs));

        int count = 0;
        if (pcnt_unit_get_count(s_unit, &count) != ESP_OK) {
            continue;
        }
        // Рахуємо за накопиченою різницею лічильника, а не за перериваннями.
        int diff = count - lastCount;
        lastCount = count;
        if (diff == 0) {
            continue;
        }
        if (cfg::kEncInvertDirection) {
            diff = -diff;
        }

        // Квантування по detent із збереженням залишку.
        remainder += diff;
        const int steps = remainder / cfg::kEncCountsPerDetent;  // до нуля
        if (steps == 0) {
            continue;
        }
        remainder -= steps * cfg::kEncCountsPerDetent;

        const int8_t dir = (steps > 0) ? 1 : -1;
        int magnitude = abs(steps);

        // Акселерація: швидке обертання в той самий бік множить крок.
        // Зміна напрямку акселерацію скидає.
        const uint32_t now = millis();
        if (cfg::kEncAccelEnabled && dir == lastDir &&
            (now - lastEventMs) < cfg::kEncAccelThresholdMs) {
            magnitude *= cfg::kEncAccelMultiplier;
        }
        lastDir = dir;
        lastEventMs = now;

        if (magnitude > INT8_MAX) {
            magnitude = INT8_MAX;
        }
        emit(dir > 0, static_cast<int8_t>(magnitude));
    }
}

}  // namespace

bool Encoder::begin() {
    if (s_task != nullptr) {
        return true;  // вже запущено
    }
    if (!EventBus::isReady()) {
        Serial.println("[ENC] begin failed: EventBus not ready");
        return false;
    }

    if (!createPcnt()) {
        destroyPcnt();
        return false;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        encoderTask, "encoder", cfg::kEncTaskStackBytes, nullptr,
        cfg::kEncTaskPriority, &s_task, cfg::kEncTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        destroyPcnt();
        Serial.println("[ENC] begin failed: task create");
        return false;
    }
    return true;
}

uint32_t Encoder::droppedEvents() {
    return s_dropped.load();
}
