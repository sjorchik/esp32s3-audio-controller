// src/usb_host_test.cpp
// ТИМЧАСОВО: тест USB host + MSC (без аудіо).
// Мета: монтування в /usb, список файлів, вимір читання (швидкість,
// гістограма затримок fread, помилки), вільна внутрішня RAM.
// Додано: DEBUG-логи драйвера MSC / FatFS для діагностики монтування.
// Видалити або вимкнути після перевірки.

#include <Arduino.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

namespace {

constexpr const char* kTag = "[USB]";
constexpr const char* kMountPath = "/usb";
constexpr size_t kEventQueueLen = 4;
constexpr int kListMaxFiles = 500;
constexpr size_t kReadBlockBytes = 4096;
constexpr size_t kReadTestMaxBytes = 8 * 1024 * 1024;

// Межі гістограми затримок fread, мкс: <5, <20, <50, <100, <200 мс, >=200 мс.
constexpr uint32_t kHistEdgesUs[] = {5000, 20000, 50000, 100000, 200000};
constexpr size_t kHistEdgeCount = sizeof(kHistEdgesUs) / sizeof(kHistEdgesUs[0]);
constexpr size_t kHistBuckets = kHistEdgeCount + 1;

// Подія MSC, яку передаємо з callback-а драйвера в задачу loop().
struct MscEvent {
    int type;      // msc_host_event_t::event (MSC_DEVICE_CONNECTED / MSC_DEVICE_DISCONNECTED)
    uint8_t addr;  // адреса USB-пристрою (для CONNECTED)
};

msc_host_device_handle_t s_msc_dev = nullptr;
msc_host_vfs_handle_t s_vfs = nullptr;
QueueHandle_t s_msc_events = nullptr;

// Callback драйвера MSC. Викликається з фонової задачі драйвера.
// Лише кладе подію в чергу, без роботи з файловою системою.
void msc_event_cb(const msc_host_event_t* event, void* arg) {
    (void)arg;
    MscEvent e{};
    e.type = static_cast<int>(event->event);
    e.addr = event->device.address;
    xQueueSend(s_msc_events, &e, 0);
}

// Задача, яка обробляє події USB Host Library.
void usb_lib_task(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

// Виводить імена файлів і каталогів у корені /usb.
void list_files() {
    DIR* dir = opendir(kMountPath);
    if (dir == nullptr) {
        Serial.printf("%s opendir(%s) failed\n", kTag, kMountPath);
        return;
    }

    int count = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr && count < kListMaxFiles) {
        Serial.printf("%s   %s\n", kTag, entry->d_name);
        count++;
    }
    closedir(dir);
    Serial.printf("%s files listed: %d\n", kTag, count);
}

// Знаходить перший файл з розширенням .mp3 у корені /usb.
bool find_first_mp3(char* out, size_t cap) {
    DIR* dir = opendir(kMountPath);
    if (dir == nullptr) {
        return false;
    }

    bool found = false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0) {
            snprintf(out, cap, "%s", entry->d_name);
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

// Номер кошика гістограми для затримки dt (мкс).
size_t hist_bucket(uint32_t dt) {
    for (size_t i = 0; i < kHistEdgeCount; i++) {
        if (dt < kHistEdgesUs[i]) {
            return i;
        }
    }
    return kHistBuckets - 1;
}

// Вимір читання одного файлу блоками. Виводить:
// - рядок зі швидкістю і максимальною затримкою;
// - гістограму затримок fread;
// - підсумковий рядок RESULT для порівняння носіїв.
void measure_read(const char* filename, uint8_t addr) {
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", kMountPath, filename);

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        Serial.printf("%s fopen(%s) failed, errno=%d\n", kTag, path, errno);
        return;
    }

    static uint8_t buf[kReadBlockBytes];
    size_t total = 0;
    uint32_t maxUs = 0;
    uint32_t blocks = 0;
    uint32_t errors = 0;
    uint32_t hist[kHistBuckets] = {0};

    uint32_t t0 = micros();
    while (total < kReadTestMaxBytes) {
        uint32_t tb = micros();
        size_t n = fread(buf, 1, kReadBlockBytes, f);
        uint32_t dt = micros() - tb;

        blocks++;
        if (dt > maxUs) {
            maxUs = dt;
        }
        hist[hist_bucket(dt)]++;

        if (n == 0) {
            if (!feof(f)) {
                errors++;
                Serial.printf("%s fread error at byte %u, errno=%d\n", kTag,
                              (unsigned)total, errno);
            }
            break;
        }
        total += n;
    }
    uint32_t elapsedUs = micros() - t0;
    fclose(f);

    float kbps = 0.0f;
    if (elapsedUs > 0) {
        kbps = (total * 8.0f / 1000.0f) / (elapsedUs / 1e6f);
    }

    Serial.printf("%s read %u bytes in %u us, %.0f kbit/s, max fread %u us, blocks %u, errors %u\n",
                  kTag, (unsigned)total, (unsigned)elapsedUs, kbps,
                  (unsigned)maxUs, (unsigned)blocks, (unsigned)errors);
    Serial.printf("%s hist <5ms:%u <20ms:%u <50ms:%u <100ms:%u <200ms:%u >=200ms:%u\n", kTag,
                  (unsigned)hist[0], (unsigned)hist[1], (unsigned)hist[2],
                  (unsigned)hist[3], (unsigned)hist[4], (unsigned)hist[5]);
    Serial.printf("%s RESULT addr=%u file=%s bytes=%u kbit=%.0f max_us=%u "
                  ">=200ms=%u >=100ms=%u errors=%u\n",
                  kTag, (unsigned)addr, filename, (unsigned)total, kbps,
                  (unsigned)maxUs, (unsigned)hist[5], (unsigned)(hist[4] + hist[5]),
                  (unsigned)errors);
}

// Вільна внутрішня RAM (важлива для планування буферів і режимів).
void print_internal_ram(const char* when) {
    Serial.printf("%s free internal (%s)=%u largest=%u\n", kTag, when,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Монтування флешки після події CONNECTED.
void handle_connected(uint8_t addr) {
    Serial.printf("%s device connected, addr=%u\n", kTag, (unsigned)addr);

    esp_err_t err = msc_host_install_device(addr, &s_msc_dev);
    Serial.printf("%s msc_host_install_device: %s\n", kTag, esp_err_to_name(err));
    if (err != ESP_OK) {
        return;
    }

    // Конфіг монтування обов'язковий (NULL дає ESP_ERR_INVALID_ARG).
    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;  // не форматувати носій користувача
    mount_cfg.max_files = 4;
    mount_cfg.allocation_unit_size = 0;        // 0 = значення за замовчуванням

    err = msc_host_vfs_register(s_msc_dev, kMountPath, &mount_cfg, &s_vfs);
    Serial.printf("%s msc_host_vfs_register: %s (0x%x)\n", kTag,
                  esp_err_to_name(err), (unsigned)err);
    if (err != ESP_OK) {
        // Пристрій встановлено, але не змонтовано: звільняємо його.
        msc_host_uninstall_device(s_msc_dev);
        s_msc_dev = nullptr;
        return;
    }

    list_files();

    char mp3[160] = {0};
    if (find_first_mp3(mp3, sizeof(mp3))) {
        print_internal_ram("before read");
        measure_read(mp3, addr);
        print_internal_ram("after read");
    } else {
        Serial.printf("%s no .mp3 files in root, read test skipped\n", kTag);
    }
}

// Розмонтування після події DISCONNECTED. Порядок: спершу VFS, потім пристрій.
void handle_disconnected() {
    Serial.printf("%s device disconnected\n", kTag);

    if (s_vfs != nullptr) {
        esp_err_t err = msc_host_vfs_unregister(s_vfs);
        Serial.printf("%s msc_host_vfs_unregister: %s\n", kTag, esp_err_to_name(err));
        s_vfs = nullptr;
    }
    if (s_msc_dev != nullptr) {
        esp_err_t err = msc_host_uninstall_device(s_msc_dev);
        Serial.printf("%s msc_host_uninstall_device: %s\n", kTag, esp_err_to_name(err));
        s_msc_dev = nullptr;
    }
}

}  // namespace

// Ініціалізація USB host і MSC драйвера. Викликається з setup().
void usb_host_test_begin() {
    // Діагностика монтування: рядки від драйвера MSC, FatFS (vfs_fat) і диск-драйвера.
    esp_log_level_set("msc_host", ESP_LOG_DEBUG);
    esp_log_level_set("vfs_fat", ESP_LOG_DEBUG);
    esp_log_level_set("diskio_usb", ESP_LOG_DEBUG);

    s_msc_events = xQueueCreate(kEventQueueLen, sizeof(MscEvent));
    if (s_msc_events == nullptr) {
        Serial.printf("%s event queue alloc failed\n", kTag);
        return;
    }

    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.root_port_unpowered = false;  // живлення порту вмикає бібліотека
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&host_cfg);
    Serial.printf("%s usb_host_install: %s\n", kTag, esp_err_to_name(err));
    if (err != ESP_OK) {
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                                            nullptr, 3, nullptr, 0);
    if (ok != pdPASS) {
        Serial.printf("%s usb_lib task create failed\n", kTag);
        return;
    }

    msc_host_driver_config_t msc_cfg = {};
    msc_cfg.create_backround_task = true;  // MSC сам створює фонову задачу подій
    msc_cfg.task_priority = 3;
    msc_cfg.stack_size = 4096;
    msc_cfg.callback = msc_event_cb;
    msc_cfg.callback_arg = nullptr;
    err = msc_host_install(&msc_cfg);
    Serial.printf("%s msc_host_install: %s\n", kTag, esp_err_to_name(err));
}

// Опитування черги подій. Викликається з loop() у кожній ітерації.
void usb_host_test_loop() {
    if (s_msc_events == nullptr) {
        return;
    }

    MscEvent e{};
    if (xQueueReceive(s_msc_events, &e, 0) != pdTRUE) {
        return;
    }

    if (e.type == msc_host_event_t::MSC_DEVICE_CONNECTED) {
        handle_connected(e.addr);
    } else if (e.type == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
        handle_disconnected();
    }
}