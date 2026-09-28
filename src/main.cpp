// Точка входу скелета проекту.
// Завдання цього файла:
// - ініціалізувати Serial;
// - надрукувати інформацію про чіп, PSRAM, flash і розділи;
// - ініціалізувати LittleFS (з форматуванням, якщо потрібно);
// - підготувати подієву шину.
// Дисплей, аудіо, мережа та ІЧ не реалізуються.

#include <Arduino.h>
#include <LittleFS.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "config/defaults.h"
#include "config/features.h"
#include "config/pins.h"
#include "core/events.h"

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

void setup() {
    Serial.begin(defaults::kSerialBaud);

    // Коротка пауза для стабілізації логу.
    vTaskDelay(pdMS_TO_TICKS(50));

    Serial.println("[MAIN] ESP32-S3 Audio Controller skeleton");
    Serial.println("[MAIN] Build: " __DATE__ " " __TIME__);

    printChipInfo();
    printPartitionInfo();
    initLittleFs();

    if (!EventBus::begin()) {
        Serial.println("[MAIN] EventBus init failed");
    } else {
        Serial.println("[MAIN] EventBus ready");
    }

    Serial.println("[MAIN] Skeleton ready");
}

void loop() {
    // Скелет ще не має активної логіки.
    // Використовуємо затримку в стилі FreeRTOS,
    // щоб не крутити порожній цикл без потреби.
    vTaskDelay(pdMS_TO_TICKS(1000));
}