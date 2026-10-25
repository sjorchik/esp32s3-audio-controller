// net/web_trace.cpp ([W1]): трасування веб-запитів. Див. web_trace.h.

#include "net/web_trace.h"

#if WEB_TRACE

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <atomic>
#include <esp_wifi.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio/audio_player.h"

namespace {

struct TraceLine {
    char text[web_cfg::kTraceLineBytes];
};

QueueHandle_t s_queue = nullptr;
std::atomic<uint32_t> s_dropped{0};
bool s_started = false;

// Виконується в задачі async_tcp: лише snprintf і xQueueSend з нульовим очікуванням.
void onRequest(AsyncWebServerRequest* req, ArMiddlewareNext next) {
    const uint32_t t0 = millis();
    next();
    const uint32_t dt = millis() - t0;
    int code = 0;
    AsyncWebServerResponse* res = req->getResponse();
    if (res != nullptr) {
        code = res->code();
    }
    TraceLine l;
    snprintf(l.text, sizeof(l.text), "[WEB] t=%lu %s %s %d %lu ms", static_cast<unsigned long>(t0),
             req->methodToString(), req->url().c_str(), code, static_cast<unsigned long>(dt));
    if (xQueueSend(s_queue, &l, 0) != pdTRUE) {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// [W1b] Одноразовий список усіх задач: ім'я, ядро, пріоритет, мінімальний вільний стек.
// Потрібен, щоб побачити, де працює внутрішня задача декодування ESP32-audioI2S ("start audio task").
void dumpTasks() {
    const UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t* arr = static_cast<TaskStatus_t*>(malloc(n * sizeof(TaskStatus_t)));
    if (arr == nullptr) {
        return;
    }
    const UBaseType_t got = uxTaskGetSystemState(arr, n, nullptr);
    for (UBaseType_t i = 0; i < got; ++i) {
        const int core = (arr[i].xCoreID == 0 || arr[i].xCoreID == 1) ? static_cast<int>(arr[i].xCoreID) : -1;
        Serial.printf("[TASK] %-16s core=%d prio=%u stackFreeMin=%u\n", arr[i].pcTaskName, core,
                      static_cast<unsigned>(arr[i].uxCurrentPriority),
                      static_cast<unsigned>(arr[i].usStackHighWaterMark));
    }
    free(arr);
}

void traceTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(web_cfg::kTraceProbeDelayMs));
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps) == ESP_OK) {
        Serial.printf("[WEB] wifi ps=%d (0 = NONE, 1 = MIN_MODEM, 2 = MAX_MODEM)\n", static_cast<int>(ps));
    }
    TaskHandle_t h = xTaskGetHandle("async_tcp");
    if (h != nullptr) {
        Serial.printf("[WEB] async_tcp core=%d prio=%u (target: core 0, prio 2)\n",
                      static_cast<int>(xTaskGetCoreID(h)), static_cast<unsigned>(uxTaskPriorityGet(h)));
    } else {
        Serial.println("[WEB] async_tcp task not found");
    }
    dumpTasks();
    TraceLine l;
    uint32_t reported = 0;
    uint32_t underruns = AudioPlayer::bufferUnderrunCount();
    for (;;) {
        // [W1b] 100 мс: і черга рядків, і спостереження за недовантаженням буфера плеєра.
        if (xQueueReceive(s_queue, &l, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.println(l.text);
        }
        const uint32_t u = AudioPlayer::bufferUnderrunCount();
        if (u != underruns) {
            Serial.printf("[WEB] t=%lu player buffer underrun total=%lu state=%s\n",
                          static_cast<unsigned long>(millis()), static_cast<unsigned long>(u),
                          AudioPlayer::stateName(AudioPlayer::state()));
            underruns = u;
        }
        const uint32_t d = s_dropped.load(std::memory_order_relaxed);
        if (d != reported) {
            Serial.printf("[WEB] trace dropped %lu line(s) in total\n", static_cast<unsigned long>(d));
            reported = d;
        }
    }
}

}  // namespace

namespace web_trace {

void begin(AsyncWebServer& server) {
    if (s_started) {
        return;
    }
    s_queue = xQueueCreate(web_cfg::kTraceQueueDepth, sizeof(TraceLine));
    if (s_queue == nullptr) {
        Serial.println("[WEB] trace queue create failed");
        return;
    }
    const BaseType_t ok = xTaskCreatePinnedToCore(traceTask, "web_trace", web_cfg::kTraceTaskStackBytes,
                                                  nullptr, web_cfg::kTraceTaskPriority, nullptr,
                                                  web_cfg::kTraceTaskCore);
    if (ok != pdPASS) {
        Serial.println("[WEB] trace task create failed");
        return;
    }
    server.addMiddleware(onRequest);
    s_started = true;
}

}  // namespace web_trace

#endif  // WEB_TRACE