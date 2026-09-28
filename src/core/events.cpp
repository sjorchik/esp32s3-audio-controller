// Мінімальна реалізація подієвої шини для скелета.
// Логіка маршрутизації буде додана в AppController.

#include "core/events.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "config/defaults.h"

static QueueHandle_t s_eventQueue = nullptr;

bool EventBus::begin() {
    if (s_eventQueue != nullptr) {
        return true;
    }

    s_eventQueue = xQueueCreate(defaults::kEventQueueSize, sizeof(Event));
    return s_eventQueue != nullptr;
}

bool EventBus::post(const Event& event, TickType_t timeout) {
    if (s_eventQueue == nullptr) {
        return false;
    }

    return xQueueSend(s_eventQueue, &event, timeout) == pdTRUE;
}

bool EventBus::poll(Event& event, TickType_t timeout) {
    if (s_eventQueue == nullptr) {
        return false;
    }

    return xQueueReceive(s_eventQueue, &event, timeout) == pdTRUE;
}

bool EventBus::isReady() {
    return s_eventQueue != nullptr;
}