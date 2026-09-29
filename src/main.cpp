// Точка входу скелета проекту.
// Завдання цього файла:
// - ініціалізувати Serial;
// - надрукувати інформацію про чіп, PSRAM, flash і розділи;
// - ініціалізувати LittleFS (з форматуванням, якщо потрібно);
// - підготувати подієву шину;
// - запустити кнопки й енкодер і (тимчасово) друкувати їхні події.
// Дисплей, аудіо, мережа та ІЧ не реалізуються.

#include <Arduino.h>
#include <LittleFS.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "config/defaults.h"
#include "config/features.h"
#include "config/input_config.h"
#include "config/pins.h"
#include "core/events.h"
#include "input/buttons.h"
#include "input/encoder.h"

// Друк інформації про чіп і памʼять.
static void printChipInfo() {
    Serial.printf("[MAIN] Chip: %s rev%d, cores=%d\n",
                  ESP.getChipModel(),
                  ESP.getChipRevision(),
                  ESP.getChipCores());

    Serial.printf("[MAIN] Flash: %u MB\n",
                  static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)));

    Serial.printf("[MAIN] PSRAM: %u bytes\n",
                  static_cast<unsigned>(ESP.getPsramSize()));

    if (!psramFound()) {
        Serial.println("[MAIN] PSRAM not found");
    }

    Serial.printf("[MAIN] Free heap: %u bytes\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));

#if ENABLE_PCM1808
    Serial.println("[MAIN] Feature PCM1808 enabled");
#else
    Serial.println("[MAIN] Feature PCM1808 disabled");
#endif

#if ENABLE_BT_UART
    Serial.println("[MAIN] Feature BT-UART enabled");
#else
    Serial.println("[MAIN] Feature BT-UART disabled");
#endif

#if ENABLE_VU
    Serial.println("[MAIN] Feature VU enabled");
#endif
}

// Друк таблиці розділів і поточної активної партіції.
static void printPartitionInfo() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        Serial.printf("[MAIN] Running partition: %s\n", running->label);
    }

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);

    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        if (part != nullptr) {
            Serial.printf("[MAIN]   partition: %-10s offset=0x%08lx size=0x%08lx\n",
                          part->label,
                          static_cast<unsigned long>(part->address),
                          static_cast<unsigned long>(part->size));
        }
        it = esp_partition_next(it);
    }

    esp_partition_iterator_release(it);
}

// Ініціалізація LittleFS з автоформатуванням.
// Якщо монтування не вдалося (наприклад, розділ порожній або пошкоджений),
// намагаємося відформатувати і змонтувати знову.
static void initLittleFs() {
    // Спроба 1: монтування без форматування.
    if (LittleFS.begin(false, defaults::kLittleFsMountPoint)) {
        Serial.printf("[MAIN] LittleFS mounted at %s\n", defaults::kLittleFsMountPoint);
        Serial.printf("[MAIN] LittleFS total: %u bytes\n",
                      static_cast<unsigned>(LittleFS.totalBytes()));
        Serial.printf("[MAIN] LittleFS used: %u bytes\n",
                      static_cast<unsigned>(LittleFS.usedBytes()));
        return;
    }

    // Спроба 2: форматування і монтування.
    Serial.println("[MAIN] LittleFS mount failed, formatting...");
    if (!LittleFS.begin(true, defaults::kLittleFsMountPoint)) {
        Serial.println("[MAIN] LittleFS format and mount failed");
        return;
    }

    Serial.printf("[MAIN] LittleFS formatted and mounted at %s\n", defaults::kLittleFsMountPoint);
    Serial.printf("[MAIN] LittleFS total: %u bytes\n",
                  static_cast<unsigned>(LittleFS.totalBytes()));
    Serial.printf("[MAIN] LittleFS used: %u bytes\n",
                  static_cast<unsigned>(LittleFS.usedBytes()));
}

#if INPUT_DEMO_PRINT_EVENTS
// Тимчасово: імена для друку подій з EventBus.
// Прибрати разом з INPUT_DEMO_PRINT_EVENTS, коли зʼявиться AppController.
static const char* actionName(Action a) {
    switch (a) {
        case Action::POWER:     return "POWER";
        case Action::UP:        return "UP";
        case Action::DOWN:      return "DOWN";
        case Action::LEFT:      return "LEFT";
        case Action::RIGHT:     return "RIGHT";
        case Action::OK:        return "OK";
        case Action::ENC_CW:    return "ENC_CW";
        case Action::ENC_CCW:   return "ENC_CCW";
        case Action::ENC_PRESS: return "ENC_PRESS";
    }
    return "?";
}

static const char* sourceName(EventSource s) {
    switch (s) {
        case EventSource::BUTTON:  return "BUTTON";
        case EventSource::ENCODER: return "ENCODER";
        case EventSource::IR:      return "IR";
        case EventSource::WEB:     return "WEB";
    }
    return "?";
}

static uint32_t s_lastBtnDropped = 0;
static uint32_t s_lastEncDropped = 0;
#endif

void setup() {
    Serial.begin(defaults::kSerialBaud);

    // Коротка пауза для стабілізації логу.
    vTaskDelay(pdMS_TO_TICKS(50));

    Serial.println("[MAIN] ESP32-S3 Audio Controller skeleton");
    Serial.println("[MAIN] Build: " __DATE__ " " __TIME__);

    // Утримання OK при старті = запит скидання Wi-Fi.
    // Перевірка йде ДО запуску задач і не залежить від EventBus.
    // Без утримання повертається за ~5 мс.
    // TODO (WifiManager): передати wifiResetRequested у логіку старту Wi-Fi.
    const bool wifiResetRequested =
        Buttons::isHeldAtBoot(pins::kBtnOk, input_cfg::kBootWifiResetHoldMs);
    if (wifiResetRequested) {
        Serial.println("[MAIN] OK held at boot: Wi-Fi reset requested");
    }

    printChipInfo();
    printPartitionInfo();
    initLittleFs();

    if (!EventBus::begin()) {
        Serial.println("[MAIN] EventBus init failed");
    } else {
        Serial.println("[MAIN] EventBus ready");
    }

    if (Buttons::begin()) {
        Serial.println("[MAIN] Buttons ready");
    } else {
        Serial.println("[MAIN] Buttons init failed");
    }

    if (Encoder::begin()) {
        Serial.println("[MAIN] Encoder ready");
    } else {
        Serial.println("[MAIN] Encoder init failed");
    }

    Serial.println("[MAIN] Skeleton ready");
}

void loop() {
#if INPUT_DEMO_PRINT_EVENTS
    // Тимчасово: друкуємо все, що прийшло в EventBus.
    // Замінить AppController.
    Event ev;
    if (EventBus::poll(ev, pdMS_TO_TICKS(1000))) {
        Serial.printf("[MAIN] event: %s %s repeat=%d long=%d delta=%d\n",
                      sourceName(ev.source), actionName(ev.action),
                      ev.repeat ? 1 : 0, ev.longPress ? 1 : 0,
                      static_cast<int>(ev.delta));
    }

    // Діагностика втрат: друкуємо лише коли лічильники змінилися.
    const uint32_t btnDropped = Buttons::droppedEvents();
    const uint32_t encDropped = Encoder::droppedEvents();
    if (btnDropped != s_lastBtnDropped || encDropped != s_lastEncDropped) {
        s_lastBtnDropped = btnDropped;
        s_lastEncDropped = encDropped;
        Serial.printf("[MAIN] events dropped: buttons=%u encoder=%u\n",
                      static_cast<unsigned>(btnDropped),
                      static_cast<unsigned>(encDropped));
    }
#else
    // Скелет ще не має активної логіки.
    // Використовуємо затримку в стилі FreeRTOS,
    // щоб не крутити порожній цикл без потреби.
    vTaskDelay(pdMS_TO_TICKS(1000));
#endif
}