// [USB2] Постійний модуль монтування USB-флешки (USB-host MSC). Замінює src/usb_host_test.*.
// Лише монтування / розмонтування й діагностика. Нічого не пишемо на носій, не форматуємо.
//
// Архітектура:
//  - msc_event_cb (задача драйвера MSC) лише ставить прапорці й кладе подію в чергу (без блокувань);
//  - задача usb_mgr (ядро 0) виконує usb_host_install, створює задачу usb_lib, ставить драйвер MSC,
//    а далі обробляє події: монтує (msc_host_install_device + msc_host_vfs_register) і розмонтовує
//    (msc_host_vfs_unregister + msc_host_uninstall_device); колбек користувача — теж звідси;
//  - s_ioMutex не дає розмонтувати том під час `usb.ls`; прапорець s_abortIo (ставиться одразу при
//    вийнятті носія) перериває довгі операції читання.

#include "usb/usb_msc_mount.h"

#if FEATURE_USB_HOST

#include <Arduino.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <atomic>

#include "config/usb_host_config.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

namespace usb_msc_mount {
namespace {

constexpr const char* kTag = "[USB]";

enum class MsgType : uint8_t { Connected, Disconnected, CmdCycle };

struct Msg {
    MsgType type;
    uint8_t addr;   // Connected: адреса USB-пристрою
    uint16_t arg;   // CmdCycle: кількість циклів
};

// --- Стан модуля ------------------------------------------------------------------------
std::atomic<bool> s_begun{false};       // begin() уже викликали
std::atomic<bool> s_installed{false};   // USB-host + MSC встановлено
std::atomic<State> s_state{State::NoDevice};
std::atomic<bool> s_present{false};     // пристрій фізично підключено (за подіями драйвера)
std::atomic<bool> s_abortIo{false};     // носій вийнято: переривати довгі читання
std::atomic<uint32_t> s_dropped{0};     // події, що не влізли в чергу

QueueHandle_t s_queue = nullptr;
SemaphoreHandle_t s_ioMutex = nullptr;       // охороняє s_dev / s_vfs від розмонтування під час usb.ls
SemaphoreHandle_t s_installDone = nullptr;   // begin() чекає завершення інсталяції в usb_mgr
TaskHandle_t s_mgrTask = nullptr;
TaskHandle_t s_libTask = nullptr;

// Лише в задачі usb_mgr (і під s_ioMutex при зміні).
msc_host_device_handle_t s_dev = nullptr;
msc_host_vfs_handle_t s_vfs = nullptr;
uint8_t s_addr = 0;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
LastError s_lastErr = {0, "none"};
EventCallback s_cb = nullptr;
void* s_cbArg = nullptr;

void setError(esp_err_t code, const char* where) {
    portENTER_CRITICAL(&s_mux);
    s_lastErr.code = static_cast<int32_t>(code);
    s_lastErr.where = where;
    portEXIT_CRITICAL(&s_mux);
}

void setState(State s) {
    const State prev = s_state.exchange(s);
    if (prev != s) {
        Serial.printf("%s state %s -> %s\n", kTag, stateName(prev), stateName(s));
    }
}

void emit(Event ev) {
    EventCallback cb = nullptr;
    void* arg = nullptr;
    portENTER_CRITICAL(&s_mux);
    cb = s_cb;
    arg = s_cbArg;
    portEXIT_CRITICAL(&s_mux);
    if (cb != nullptr) {
        cb(ev, arg);
    }
}

#if USB_HOST_DIAG
void printHeap(const char* label) {
    Serial.printf("%s heap[%s] internal free=%u largest=%u min_free=%u psram_free=%u state=%s\n", kTag,
                  label, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), stateName(s_state.load()));
}
#endif

// --- Колбек драйвера MSC (задача драйвера; без блокувань і без роботи з FS) -------------------
void mscEventCb(const msc_host_event_t* event, void* arg) {
    (void)arg;
    Msg m = {};
    if (event->event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
        m.type = MsgType::Connected;
        m.addr = event->device.address;
        s_present.store(true);
    } else if (event->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
        m.type = MsgType::Disconnected;
        s_present.store(false);
        s_abortIo.store(true);  // одразу перервати довгі читання (usb.ls)
    } else {
        return;  // SUSPENDED / RESUMED (якщо збірка їх має) — не використовуємо
    }
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        s_dropped.fetch_add(1);
    }
}

// Задача, яка обробляє події USB Host Library. [USB1]
void libTask(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        const esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));  // не крутитись на помилці
        }
    }
}

// --- Монтування / розмонтування ---------------------------------------------------------------
esp_err_t doMount(uint8_t addr) {
    esp_err_t err = msc_host_install_device(addr, &s_dev);
    Serial.printf("%s msc_host_install_device: %s (0x%x)\n", kTag, errorName(err), (unsigned)err);
    if (err != ESP_OK) {
        s_dev = nullptr;
        setError(err, "install_device");
        return err;
    }

    // Конфіг монтування обовʼязковий і НЕ порожній (NULL дає ESP_ERR_INVALID_ARG). [USB1]
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = usb_host_cfg::kFormatIfMountFailed;  // false: НЕ форматувати носій
    cfg.max_files = usb_host_cfg::kMaxFiles;
    cfg.allocation_unit_size = usb_host_cfg::kAllocationUnitSize;

    err = msc_host_vfs_register(s_dev, usb_host_cfg::kMountPath, &cfg, &s_vfs);
    Serial.printf("%s msc_host_vfs_register: %s (0x%x)\n", kTag, errorName(err), (unsigned)err);
    if (err != ESP_OK) {
        s_vfs = nullptr;
        const esp_err_t uerr = msc_host_uninstall_device(s_dev);  // пристрій встановлено, але не змонтовано
        Serial.printf("%s msc_host_uninstall_device: %s\n", kTag, errorName(uerr));
        s_dev = nullptr;
        setError(err, "vfs_register");
        return err;
    }
    return ESP_OK;
}

// Порядок: спершу VFS, потім пристрій. Тримає s_ioMutex: чекає завершення usb.ls (яке перериває s_abortIo).
void unmountLocked() {
    xSemaphoreTake(s_ioMutex, portMAX_DELAY);
    if (s_vfs != nullptr) {
        const esp_err_t err = msc_host_vfs_unregister(s_vfs);
        Serial.printf("%s msc_host_vfs_unregister: %s\n", kTag, errorName(err));
        if (err != ESP_OK) {
            setError(err, "vfs_unregister");
        }
        s_vfs = nullptr;
    }
    if (s_dev != nullptr) {
        const esp_err_t err = msc_host_uninstall_device(s_dev);
        Serial.printf("%s msc_host_uninstall_device: %s\n", kTag, errorName(err));
        if (err != ESP_OK) {
            setError(err, "uninstall_device");
        }
        s_dev = nullptr;
    }
    xSemaphoreGive(s_ioMutex);
}

// Монтування з повторами. true = змонтовано. При невдачі: Error (якщо носій на місці) або стан
// лишається Mounting до обробки queued-події Disconnected.
bool mountWithRetry(uint8_t addr) {
    s_abortIo.store(false);
    setState(State::Mounting);
    for (uint8_t attempt = 1; attempt <= usb_host_cfg::kMountMaxAttempts; ++attempt) {
        if (!s_present.load()) {
            break;
        }
        Serial.printf("%s mount attempt %u/%u addr=%u\n", kTag, (unsigned)attempt,
                      (unsigned)usb_host_cfg::kMountMaxAttempts, (unsigned)addr);
        const uint32_t t0 = millis();
        const esp_err_t err = doMount(addr);
        const uint32_t dt = millis() - t0;
        if (err == ESP_OK) {
            Serial.printf("%s mounted at %s in %u ms (free internal=%u largest=%u)\n", kTag,
                          usb_host_cfg::kMountPath, (unsigned)dt,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (dt > usb_host_cfg::kMountSlowWarnMs) {
                Serial.printf("%s WARNING: mount took %u ms (> %u ms)\n", kTag, (unsigned)dt,
                              (unsigned)usb_host_cfg::kMountSlowWarnMs);
            }
            setState(State::Mounted);
            emit(Event::Mounted);
            return true;
        }
        Serial.printf("%s mount failed after %u ms: %s (0x%x)\n", kTag, (unsigned)dt, errorName(err),
                      (unsigned)err);
        if (attempt < usb_host_cfg::kMountMaxAttempts) {
            vTaskDelay(pdMS_TO_TICKS(usb_host_cfg::kMountRetryDelayMs));
        }
    }
    if (s_present.load()) {
        setState(State::Error);
        emit(Event::Error);
    }
    return false;
}

void handleConnected(uint8_t addr) {
    if (s_dev != nullptr || s_vfs != nullptr) {
        Serial.printf("%s connect event ignored: already mounted\n", kTag);
        return;
    }
    if (!s_present.load()) {
        Serial.printf("%s stale connect event ignored (device already gone)\n", kTag);
        return;
    }
    s_addr = addr;
    Serial.printf("%s device inserted addr=%u\n", kTag, (unsigned)addr);
    setState(State::Mounting);
    emit(Event::Inserted);
    mountWithRetry(addr);
}

void handleDisconnected() {
    const State prev = s_state.load();
    const bool had = (s_dev != nullptr || s_vfs != nullptr);
    Serial.printf("%s device removed\n", kTag);
    unmountLocked();
    setState(State::NoDevice);
    if (had || prev != State::NoDevice) {
        emit(Event::Removed);
    }
}

#if USB_HOST_DIAG
// usb.cycle N: програмне розмонтування / монтування без фізичного вийняття. Чи дозволяє це драйвер —
// НЕ перевірено: якщо msc_host_install_device на тій самій адресі не спрацює, цикл зупиняється з повідомленням.
void runCycle(uint16_t n) {
    if (n == 0) {
        return;
    }
    if (s_state.load() != State::Mounted || !s_present.load()) {
        Serial.printf("%s cycle: need a mounted device (state=%s)\n", kTag, stateName(s_state.load()));
        return;
    }
    const uint8_t addr = s_addr;
    printHeap("cycle start");
    const uint32_t free0 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t okCount = 0;
    for (uint16_t i = 1; i <= n; ++i) {
        if (!s_present.load()) {
            Serial.printf("%s cycle %u/%u aborted: device removed\n", kTag, (unsigned)i, (unsigned)n);
            break;
        }
        s_abortIo.store(true);
        unmountLocked();
        setState(State::NoDevice);
        emit(Event::Removed);
        vTaskDelay(pdMS_TO_TICKS(usb_host_cfg::kCycleGapMs));
        if (!s_present.load()) {
            Serial.printf("%s cycle %u/%u aborted: device removed\n", kTag, (unsigned)i, (unsigned)n);
            break;
        }
        emit(Event::Inserted);
        if (!mountWithRetry(addr)) {
            Serial.printf("%s cycle %u/%u FAILED (last error 0x%x)%s\n", kTag, (unsigned)i, (unsigned)n,
                          (unsigned)lastError().code,
                          (i == 1) ? "; software re-mount may be unsupported by the driver - re-plug the device"
                                   : "");
            break;
        }
        okCount++;
        Serial.printf("%s cycle %u/%u ok free_internal=%u largest=%u\n", kTag, (unsigned)i, (unsigned)n,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    const uint32_t free1 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    printHeap("cycle end");
    Serial.printf("%s cycle done: ok=%u/%u free_internal %u -> %u (delta %ld)\n", kTag, (unsigned)okCount,
                  (unsigned)n, (unsigned)free0, (unsigned)free1, (long)free1 - (long)free0);
}
#endif  // USB_HOST_DIAG

// Інсталяція стеку USB: виконується в задачі usb_mgr (ядро 0), щоб ISR USB виділився на ядрі 0.
esp_err_t installStack() {
#if USB_HOST_DIAG
    // Діагностика монтування: рядки драйвера MSC, FatFS і диск-драйвера. [USB1]
    esp_log_level_set("msc_host", ESP_LOG_DEBUG);
    esp_log_level_set("vfs_fat", ESP_LOG_DEBUG);
    esp_log_level_set("diskio_usb", ESP_LOG_DEBUG);
#endif

    usb_host_config_t hostCfg = {};
    hostCfg.skip_phy_setup = false;
    hostCfg.root_port_unpowered = false;  // живлення порту вмикає бібліотека [USB1]
    hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&hostCfg);
    Serial.printf("%s usb_host_install: %s (core %d)\n", kTag, errorName(err), (int)xPortGetCoreID());
    if (err != ESP_OK) {
        setError(err, "usb_host_install");
        return err;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(libTask, "usb_lib", usb_host_cfg::kLibTaskStack, nullptr,
                                                  usb_host_cfg::kLibTaskPriority, &s_libTask,
                                                  usb_host_cfg::kTaskCore);
    if (ok != pdPASS) {
        Serial.printf("%s usb_lib task create failed\n", kTag);
        setError(ESP_ERR_NO_MEM, "usb_lib_task");
        return ESP_ERR_NO_MEM;
    }

    msc_host_driver_config_t mscCfg = {};
    mscCfg.create_backround_task = true;  // (sic) так названо поле в msc_host.h
    mscCfg.task_priority = usb_host_cfg::kMscTaskPriority;
    mscCfg.stack_size = usb_host_cfg::kMscTaskStack;
    mscCfg.core_id = usb_host_cfg::kTaskCore;
    mscCfg.callback = mscEventCb;
    mscCfg.callback_arg = nullptr;
    err = msc_host_install(&mscCfg);
    Serial.printf("%s msc_host_install: %s (0x%x)\n", kTag, errorName(err), (unsigned)err);
    if (err != ESP_OK) {
        setError(err, "msc_host_install");
        return err;
    }
    return ESP_OK;
}

void mgrTask(void* arg) {
    (void)arg;
    const esp_err_t err = installStack();
    s_installed.store(err == ESP_OK);
    if (err == ESP_OK) {
        Serial.printf("%s ready: core=%d prio lib/msc/mgr=%u/%u/%u mount=%s\n", kTag,
                      (int)usb_host_cfg::kTaskCore, usb_host_cfg::kLibTaskPriority,
                      usb_host_cfg::kMscTaskPriority, usb_host_cfg::kMgrTaskPriority,
                      usb_host_cfg::kMountPath);
    } else {
        setState(State::Error);
    }
    xSemaphoreGive(s_installDone);
    if (err != ESP_OK) {
        s_mgrTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        Msg m = {};
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (m.type) {
            case MsgType::Connected:
                handleConnected(m.addr);
                break;
            case MsgType::Disconnected:
                handleDisconnected();
                break;
            case MsgType::CmdCycle:
#if USB_HOST_DIAG
                runCycle(m.arg);
#endif
                break;
        }
    }
}

}  // namespace

// --- Публічний інтерфейс -----------------------------------------------------------------------
const char* stateName(State s) {
    switch (s) {
        case State::NoDevice: return "NoDevice";
        case State::Mounting: return "Mounting";
        case State::Mounted: return "Mounted";
        case State::Error: return "Error";
    }
    return "?";
}

const char* errorName(int32_t code) {
    switch (code) {
        case 0: return "OK";
        case ESP_ERR_MSC_MOUNT_FAILED: return "MSC_MOUNT_FAILED";
        case ESP_ERR_MSC_FORMAT_FAILED: return "MSC_FORMAT_FAILED";
        case ESP_ERR_MSC_INTERNAL: return "MSC_INTERNAL";
        case ESP_ERR_MSC_STALL: return "MSC_STALL";
        default: return esp_err_to_name(static_cast<esp_err_t>(code));
    }
}

bool begin() {
    bool expected = false;
    if (!s_begun.compare_exchange_strong(expected, true)) {
        return s_installed.load();  // повторний виклик: результат першого
    }

    s_queue = xQueueCreate(usb_host_cfg::kEventQueueLen, sizeof(Msg));
    s_ioMutex = xSemaphoreCreateMutex();
    s_installDone = xSemaphoreCreateBinary();
    if (s_queue == nullptr || s_ioMutex == nullptr || s_installDone == nullptr) {
        Serial.printf("%s alloc failed (queue/mutex)\n", kTag);
        setError(ESP_ERR_NO_MEM, "alloc");
        setState(State::Error);
        return false;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(mgrTask, "usb_mgr", usb_host_cfg::kMgrTaskStack, nullptr,
                                                  usb_host_cfg::kMgrTaskPriority, &s_mgrTask,
                                                  usb_host_cfg::kTaskCore);
    if (ok != pdPASS) {
        Serial.printf("%s usb_mgr task create failed\n", kTag);
        setError(ESP_ERR_NO_MEM, "mgr_task");
        setState(State::Error);
        return false;
    }

    if (xSemaphoreTake(s_installDone, pdMS_TO_TICKS(usb_host_cfg::kInstallWaitMs)) != pdTRUE) {
        Serial.printf("%s install wait timeout (%u ms); installation continues in background\n", kTag,
                      (unsigned)usb_host_cfg::kInstallWaitMs);
        return false;
    }
    return s_installed.load();
}

bool isMounted() { return s_state.load() == State::Mounted; }

State state() { return s_state.load(); }

LastError lastError() {
    LastError e;
    portENTER_CRITICAL(&s_mux);
    e = s_lastErr;
    portEXIT_CRITICAL(&s_mux);
    return e;
}

const char* mountPath() { return usb_host_cfg::kMountPath; }

void setEventCallback(EventCallback cb, void* arg) {
    portENTER_CRITICAL(&s_mux);
    s_cb = cb;
    s_cbArg = arg;
    portEXIT_CRITICAL(&s_mux);
}

// --- Діагностична консоль -----------------------------------------------------------------------
#if USB_HOST_DIAG
namespace {

void cmdStat() {
    const LastError e = lastError();
    Serial.printf("%s stat: begun=%d installed=%d state=%s present=%d path=%s\n", kTag, (int)s_begun.load(),
                  (int)s_installed.load(), stateName(s_state.load()), (int)s_present.load(), mountPath());
    Serial.printf("%s stat: last_error=0x%x (%s) at '%s' dropped_events=%u\n", kTag, (unsigned)e.code,
                  errorName(e.code), e.where, (unsigned)s_dropped.load());
    // IDF: uxTaskGetStackHighWaterMark повертає БАЙТИ (мінімум вільного стеку за час роботи).
    if (s_mgrTask != nullptr) {
        Serial.printf("%s stat: usb_mgr stack min free=%u of %u bytes\n", kTag,
                      (unsigned)uxTaskGetStackHighWaterMark(s_mgrTask), (unsigned)usb_host_cfg::kMgrTaskStack);
    }
    if (s_libTask != nullptr) {
        Serial.printf("%s stat: usb_lib stack min free=%u of %u bytes\n", kTag,
                      (unsigned)uxTaskGetStackHighWaterMark(s_libTask), (unsigned)usb_host_cfg::kLibTaskStack);
    }
}

void cmdLs(const char* arg) {
    if (!isMounted()) {
        Serial.printf("%s ls: not mounted (state=%s)\n", kTag, stateName(s_state.load()));
        return;
    }
    char path[usb_host_cfg::kPathMax];
    if (arg == nullptr || arg[0] == '\0') {
        snprintf(path, sizeof(path), "%s", mountPath());
    } else {
        snprintf(path, sizeof(path), "%s", arg);
    }
    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/') {
        path[--plen] = '\0';  // без кінцевого '/'
    }
    const size_t mlen = strlen(mountPath());
    if (strncmp(path, mountPath(), mlen) != 0 || (path[mlen] != '\0' && path[mlen] != '/') ||
        strstr(path, "..") != nullptr) {
        Serial.printf("%s ls: path must be under %s\n", kTag, mountPath());
        return;
    }

    if (xSemaphoreTake(s_ioMutex, pdMS_TO_TICKS(usb_host_cfg::kLsLockWaitMs)) != pdTRUE) {
        Serial.printf("%s ls: busy (mount/unmount in progress)\n", kTag);
        return;
    }
    DIR* dir = isMounted() ? opendir(path) : nullptr;
    if (dir == nullptr) {
        Serial.printf("%s ls: opendir(%s) failed, errno=%d\n", kTag, path, errno);
        xSemaphoreGive(s_ioMutex);
        return;
    }

    int count = 0;
    bool truncated = false;
    bool aborted = false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (s_abortIo.load()) {
            aborted = true;
            break;
        }
        if (count >= usb_host_cfg::kLsMaxLines) {
            truncated = true;
            break;
        }
        char full[usb_host_cfg::kPathMax];
        const int n = snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st;
        if (n > 0 && (size_t)n < sizeof(full) && stat(full, &st) == 0) {
            Serial.printf("%s   %c %10lu  %s\n", kTag, S_ISDIR(st.st_mode) ? 'd' : '-',
                          (unsigned long)st.st_size, entry->d_name);
        } else {
            Serial.printf("%s   ?  (no stat)  %s\n", kTag, entry->d_name);
        }
        count++;
    }
    closedir(dir);
    xSemaphoreGive(s_ioMutex);

    if (aborted) {
        Serial.printf("%s ls: aborted, device removed (listed %d)\n", kTag, count);
    } else if (truncated) {
        Serial.printf("%s ls: listed %d (limit %d reached)\n", kTag, count, usb_host_cfg::kLsMaxLines);
    } else {
        Serial.printf("%s ls: listed %d\n", kTag, count);
    }
}

void cmdCycle(const char* arg) {
    if (!s_installed.load()) {
        Serial.printf("%s cycle: USB host not started (usb.begin)\n", kTag);
        return;
    }
    const long n = (arg != nullptr) ? strtol(arg, nullptr, 10) : 0;
    if (n < 1 || (unsigned long)n > usb_host_cfg::kCycleMaxCount) {
        Serial.printf("%s cycle: usage usb.cycle N (1..%u)\n", kTag, (unsigned)usb_host_cfg::kCycleMaxCount);
        return;
    }
    Msg m = {};
    m.type = MsgType::CmdCycle;
    m.arg = static_cast<uint16_t>(n);
    if (xQueueSend(s_queue, &m, 0) == pdTRUE) {
        Serial.printf("%s cycle: %ld cycles queued, results follow\n", kTag, n);
    } else {
        Serial.printf("%s cycle: queue full\n", kTag);
    }
}

// Пропуск пробілів на початку аргументу.
const char* skipSpaces(const char* s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

bool isCmd(const char* line, const char* name) {
    const size_t n = strlen(name);
    return strncmp(line, name, n) == 0 && (line[n] == '\0' || line[n] == ' ' || line[n] == '\t');
}

void handleLine(const char* line) {
    if (isCmd(line, "usb.stat")) {
        cmdStat();
    } else if (isCmd(line, "usb.heap")) {
        printHeap("manual");
    } else if (isCmd(line, "usb.ls")) {
        cmdLs(skipSpaces(line + strlen("usb.ls")));
    } else if (isCmd(line, "usb.cycle")) {
        cmdCycle(skipSpaces(line + strlen("usb.cycle")));
    } else if (isCmd(line, "usb.begin")) {
        const bool ok = begin();
        Serial.printf("%s begin: %s\n", kTag, ok ? "ok" : "failed (see usb.stat)");
    } else if (isCmd(line, "usb.help")) {
        Serial.printf("%s commands: usb.stat | usb.ls [path] | usb.cycle N | usb.heap | usb.begin\n", kTag);
    } else {
        Serial.printf("%s unknown command (usb.help)\n", kTag);
    }
}

}  // namespace

void pollConsole() {
    static char line[usb_host_cfg::kConsoleLineMax];
    static size_t len = 0;
    while (Serial.available() > 0) {
        // Не чіпаємо рядки інших консолей (xsmt.*, lp.*, st.*, set.* …): беремо байт лише якщо рядок
        // уже почато нами або починається з 'u'. (Інші консолі, що читають Serial з власних задач,
        // можуть забрати й наш рядок - див. «Відомі обмеження».)
        if (len == 0 && Serial.peek() != 'u') {
            return;
        }
        const int c = Serial.read();
        if (c < 0) {
            return;
        }
        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                len = 0;
                handleLine(line);
            }
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = static_cast<char>(c);
        }
    }
}
#endif  // USB_HOST_DIAG

}  // namespace usb_msc_mount

#endif  // FEATURE_USB_HOST
