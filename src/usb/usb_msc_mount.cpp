// [USB2] Постійний модуль монтування USB-флешки (USB-host MSC). Замінює src/usb_host_test.*.
// [USB3] ДОДАНО end() і ліниве піднімання: begin() / end() можна викликати багато разів.
// Лише монтування / розмонтування й діагностика. Нічого не пишемо на носій, не форматуємо.
//
// Архітектура:
//  - msc_event_cb (задача драйвера MSC) лише ставить прапорці й кладе подію в чергу (без блокувань);
//  - задача usb_mgr (ядро 0) виконує usb_host_install, створює задачу usb_lib, ставить драйвер MSC,
//    а далі обробляє події: монтує (msc_host_install_device + msc_host_vfs_register) і розмонтовує
//    (msc_host_vfs_unregister + msc_host_uninstall_device); колбек користувача — теж звідси;
//  - [USB3] команда CmdEnd у цій самій черзі: usb_mgr знімає стек у порядку, який вимагають заголовки
//    (usb/usb_host.h, msc_host.h, msc_host_vfs.h), і завершується сама (vTaskDelete(nullptr)):
//      1. msc_host_vfs_unregister, msc_host_uninstall_device (під s_ioMutex);
//      2. msc_host_uninstall (INVALID_STATE, якщо пристрій ще відкритий);
//      3. usb_host_lib_info: клієнтів має не лишитись (заголовок MSC не каже, що uninstall знімає клієнта —
//         ПЕРЕВІРЯЄМО, а не вгадуємо), інакше помилка без usb_host_uninstall;
//      4. usb_host_device_free_all (вимагає, щоб клієнтів не було) і очікування прапорця ALL_FREE,
//         який бачить задача usb_lib, якщо повернулось ESP_ERR_NOT_FINISHED;
//      5. зупинка usb_lib (прапорець + usb_host_lib_unblock) — вона виходить сама;
//      6. usb_host_uninstall (повтори з usb_host_lib_handle_events із usb_mgr при INVALID_STATE);
//  - s_ioMutex не дає розмонтувати том під час `usb.ls`; прапорець s_abortIo (ставиться одразу при
//    вийнятті носія й при end()) перериває довгі операції читання;
//  - черги / мьютекси / семафори — СТАТИЧНІ (xxxCreateStatic, у .bss, без heap): їх не потрібно
//    звільняти між begin() / end(), немає гонки «хтось ще тримає видалений обʼєкт», а вимір free
//    внутрішньої RAM не залежить від цих обʼєктів.

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

enum class MsgType : uint8_t { Connected, Disconnected, CmdCycle, CmdEnd };

struct Msg {
    MsgType type;
    uint8_t addr;   // Connected: адреса USB-пристрою
    uint16_t arg;   // CmdCycle: кількість циклів
};

// Життєвий цикл стеку USB-host (внутрішній; публічний State окремий).
enum class Life : uint8_t {
    Off,       // стек не піднято, задач немає
    Starting,  // begin() створив usb_mgr, інсталяція триває
    Up,        // стек піднято, usb_mgr обробляє події
    Stopping,  // end() прийнято, usb_mgr знімає стек
};

// --- Стан модуля ------------------------------------------------------------------------
std::atomic<Life> s_life{Life::Off};
std::atomic<bool> s_installed{false};   // USB-host + MSC встановлено й працює
std::atomic<State> s_state{State::Off};
std::atomic<bool> s_present{false};     // пристрій фізично підключено (за подіями драйвера)
std::atomic<bool> s_abortIo{false};     // носій вийнято / end(): переривати довгі читання
std::atomic<uint32_t> s_dropped{0};     // події, що не влізли в чергу
std::atomic<bool> s_hostUp{false};      // usb_host_install виконано й ще не знято
std::atomic<bool> s_mscUp{false};       // msc_host_install виконано й ще не знято
std::atomic<bool> s_libStop{false};     // просить usb_lib завершитись
std::atomic<bool> s_endOk{true};        // результат останнього зняття (для end() після таймауту)

// Статичні обʼєкти FreeRTOS (див. шапку файлу).
StaticQueue_t s_queueCb;
uint8_t s_queueStore[usb_host_cfg::kEventQueueLen * sizeof(Msg)];
StaticSemaphore_t s_lifeMutexCb;
StaticSemaphore_t s_ioMutexCb;
StaticSemaphore_t s_installDoneCb;
StaticSemaphore_t s_endDoneCb;
StaticSemaphore_t s_allFreeCb;
StaticSemaphore_t s_libExitedCb;

QueueHandle_t s_queue = nullptr;
SemaphoreHandle_t s_lifeMutex = nullptr;     // серіалізує begin() / end()
SemaphoreHandle_t s_ioMutex = nullptr;       // охороняє s_dev / s_vfs від розмонтування під час usb.ls
SemaphoreHandle_t s_installDone = nullptr;   // begin() чекає завершення інсталяції в usb_mgr
SemaphoreHandle_t s_endDone = nullptr;       // end() чекає завершення зняття в usb_mgr
SemaphoreHandle_t s_allFree = nullptr;       // usb_lib -> usb_mgr: подія ALL_FREE
SemaphoreHandle_t s_libExited = nullptr;     // usb_lib -> usb_mgr: задача завершується
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

// Створює статичні обʼєкти (без heap). Ідемпотентно й безпечно при гонці двох перших викликів.
void ensureObjects() {
    portENTER_CRITICAL(&s_mux);
    if (s_queue == nullptr) {
        s_queue = xQueueCreateStatic(usb_host_cfg::kEventQueueLen, sizeof(Msg), s_queueStore, &s_queueCb);
        s_lifeMutex = xSemaphoreCreateMutexStatic(&s_lifeMutexCb);
        s_ioMutex = xSemaphoreCreateMutexStatic(&s_ioMutexCb);
        s_installDone = xSemaphoreCreateBinaryStatic(&s_installDoneCb);
        s_endDone = xSemaphoreCreateBinaryStatic(&s_endDoneCb);
        s_allFree = xSemaphoreCreateBinaryStatic(&s_allFreeCb);
        s_libExited = xSemaphoreCreateBinaryStatic(&s_libExitedCb);
    }
    portEXIT_CRITICAL(&s_mux);
}

void drain(SemaphoreHandle_t sem) {
    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

const char* lifeName(Life l) {
    switch (l) {
        case Life::Off: return "Off";
        case Life::Starting: return "Starting";
        case Life::Up: return "Up";
        case Life::Stopping: return "Stopping";
    }
    return "?";
}

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
// [USB3] Скінченний таймаут і вихід за s_libStop; сама бачить ALL_FREE і передає його usb_mgr.
// Завершується сама (vTaskDelete(nullptr)), без зовнішнього vTaskDelete.
void libTask(void* arg) {
    (void)arg;
    while (!s_libStop.load()) {
        uint32_t flags = 0;
        const esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(usb_host_cfg::kLibWaitMs), &flags);
        if (err == ESP_OK) {
            if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0) {
                xSemaphoreGive(s_allFree);
            }
        } else if (err != ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(10));  // не крутитись на помилці
        }
    }
    xSemaphoreGive(s_libExited);  // після цього задача нічого не чіпає
    vTaskDelete(nullptr);
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
// [USB3] Повтори переривається запитом end() (Life::Stopping).
bool mountWithRetry(uint8_t addr) {
    s_abortIo.store(false);
    setState(State::Mounting);
    for (uint8_t attempt = 1; attempt <= usb_host_cfg::kMountMaxAttempts; ++attempt) {
        if (!s_present.load() || s_life.load() == Life::Stopping) {
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
        if (attempt < usb_host_cfg::kMountMaxAttempts && s_life.load() != Life::Stopping) {
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
    if (s_life.load() == Life::Stopping) {
        Serial.printf("%s connect event ignored: stopping\n", kTag);
        return;
    }
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
        if (!s_present.load() || s_life.load() == Life::Stopping) {
            Serial.printf("%s cycle %u/%u aborted: device removed or stopping\n", kTag, (unsigned)i, (unsigned)n);
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

// --- Інсталяція / зняття стеку ------------------------------------------------------------------
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
    s_hostUp.store(true);

    s_libStop.store(false);
    drain(s_libExited);
    drain(s_allFree);
    const BaseType_t ok = xTaskCreatePinnedToCore(libTask, "usb_lib", usb_host_cfg::kLibTaskStack, nullptr,
                                                  usb_host_cfg::kLibTaskPriority, &s_libTask,
                                                  usb_host_cfg::kTaskCore);
    if (ok != pdPASS) {
        Serial.printf("%s usb_lib task create failed\n", kTag);
        s_libTask = nullptr;
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
    s_mscUp.store(true);
    return ESP_OK;
}

// Зупиняє usb_lib: прапорець + usb_host_lib_unblock(); задача виходить сама. Ідемпотентно.
esp_err_t stopLibTask() {
    if (s_libTask == nullptr) {
        return ESP_OK;
    }
    s_libStop.store(true);
    const esp_err_t uerr = usb_host_lib_unblock();  // лише пришвидшує вихід; решту робить таймаут kLibWaitMs
    if (uerr != ESP_OK) {
        Serial.printf("%s usb_host_lib_unblock: %s\n", kTag, errorName(uerr));
    }
    if (xSemaphoreTake(s_libExited, pdMS_TO_TICKS(usb_host_cfg::kEndStepWaitMs)) != pdTRUE) {
        Serial.printf("%s usb_lib did not exit in %u ms\n", kTag, (unsigned)usb_host_cfg::kEndStepWaitMs);
        setError(ESP_ERR_TIMEOUT, "lib_task_exit");
        return ESP_ERR_TIMEOUT;
    }
    s_libTask = nullptr;
    return ESP_OK;
}

// Зняття стеку за стадіями (прапорці s_mscUp / s_hostUp / s_libTask): ідемпотентне, повторний виклик після
// помилки продовжує з місця зупинки. Викликається лише з usb_mgr, після unmountLocked().
esp_err_t teardownStack() {
    esp_err_t err = ESP_OK;

    if (s_mscUp.load()) {
        err = msc_host_uninstall();  // INVALID_STATE: драйвер не встановлено або пристрій ще відкрито
        Serial.printf("%s msc_host_uninstall: %s (0x%x)\n", kTag, errorName(err), (unsigned)err);
        if (err != ESP_OK) {
            setError(err, "msc_host_uninstall");
            return err;
        }
        s_mscUp.store(false);
    }

    if (s_hostUp.load()) {
        if (s_libTask != nullptr) {  // usb_lib ще працює: перевіряємо клієнтів і звільняємо пристрої
            usb_host_lib_info_t info = {};
            err = usb_host_lib_info(&info);
            Serial.printf("%s usb_host_lib_info: %s num_devices=%d num_clients=%d\n", kTag, errorName(err),
                          info.num_devices, info.num_clients);
            if (err == ESP_OK && info.num_clients > 0) {
                // ПРИПУЩЕННЯ перевірено: msc_host_uninstall не зняв клієнта USB Host — заголовок msc_host.h
                // цього не гарантує. usb_host_uninstall без цього заборонений, тому далі НЕ йдемо.
                Serial.printf("%s teardown stopped: %d USB host client(s) still registered\n", kTag,
                              info.num_clients);
                setError(ESP_ERR_INVALID_STATE, "client_left");
                return ESP_ERR_INVALID_STATE;
            }

            drain(s_allFree);
            err = usb_host_device_free_all();
            Serial.printf("%s usb_host_device_free_all: %s (0x%x)\n", kTag, errorName(err), (unsigned)err);
            if (err == ESP_ERR_NOT_FINISHED) {
                if (xSemaphoreTake(s_allFree, pdMS_TO_TICKS(usb_host_cfg::kEndStepWaitMs)) != pdTRUE) {
                    Serial.printf("%s ALL_FREE not received in %u ms\n", kTag,
                                  (unsigned)usb_host_cfg::kEndStepWaitMs);
                    setError(ESP_ERR_TIMEOUT, "all_free_timeout");
                    return ESP_ERR_TIMEOUT;
                }
                Serial.printf("%s USB_HOST_LIB_EVENT_FLAGS_ALL_FREE received\n", kTag);
            } else if (err != ESP_OK) {
                setError(err, "device_free_all");
                return err;
            }

            err = stopLibTask();
            if (err != ESP_OK) {
                return err;
            }
        }

        // usb_lib зупинено: події бібліотеки на час повторів обробляє usb_mgr (єдиний виклик handle_events).
        const uint32_t t0 = millis();
        uint32_t retries = 0;
        for (;;) {
            err = usb_host_uninstall();
            if (err != ESP_ERR_INVALID_STATE || (millis() - t0) >= usb_host_cfg::kEndStepWaitMs) {
                break;
            }
            ++retries;
            uint32_t flags = 0;
            (void)usb_host_lib_handle_events(pdMS_TO_TICKS(usb_host_cfg::kEndPollMs), &flags);
        }
        Serial.printf("%s usb_host_uninstall: %s (0x%x) retries=%u in %u ms\n", kTag, errorName(err),
                      (unsigned)err, (unsigned)retries, (unsigned)(millis() - t0));
        if (err != ESP_OK) {
            setError(err, "usb_host_uninstall");
            return err;
        }
        s_hostUp.store(false);
    }

    return stopLibTask();  // якщо хост уже знято, а задача лишилась (повтор після збою)
}

void mgrTask(void* arg) {
    (void)arg;
    const esp_err_t err = installStack();
    if (err != ESP_OK) {
        // Прибрати частково встановлене (usb_host_install без usb_lib / MSC тощо).
        const esp_err_t terr = teardownStack();
        Serial.printf("%s install failed, cleanup: %s\n", kTag, errorName(terr));
        s_installed.store(false);
        setState(State::Error);
        s_mgrTask = nullptr;
        const Life prev = s_life.exchange(Life::Off);
        xSemaphoreGive(s_installDone);
        if (prev == Life::Stopping) {  // end() чекає: стек не піднято, вважаємо знятим
            s_endOk.store(terr == ESP_OK);
            xSemaphoreGive(s_endDone);
        }
        vTaskDelete(nullptr);
        return;
    }

    s_installed.store(true);
    setState(State::NoDevice);
    Life expected = Life::Starting;
    s_life.compare_exchange_strong(expected, Life::Up);  // якщо вже Stopping — не затираємо
    Serial.printf("%s ready: core=%d prio lib/msc/mgr=%u/%u/%u mount=%s\n", kTag,
                  (int)usb_host_cfg::kTaskCore, usb_host_cfg::kLibTaskPriority,
                  usb_host_cfg::kMscTaskPriority, usb_host_cfg::kMgrTaskPriority,
                  usb_host_cfg::kMountPath);
    xSemaphoreGive(s_installDone);

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
            case MsgType::CmdEnd: {
                Serial.printf("%s end: stopping\n", kTag);
                s_abortIo.store(true);  // перервати usb.ls
                const bool had = (s_dev != nullptr || s_vfs != nullptr);
                unmountLocked();
                if (had) {
                    emit(Event::Removed);
                }
                const esp_err_t terr = teardownStack();
                if (terr != ESP_OK) {
                    // Стек не знято повністю: лишаємось живими (Up), end() можна повторити.
                    Serial.printf("%s end FAILED: %s (0x%x) at '%s'\n", kTag, errorName(terr), (unsigned)terr,
                                  lastError().where);
                    setState(State::Error);
                    s_endOk.store(false);
                    s_life.store(Life::Up);
                    xSemaphoreGive(s_endDone);
                    break;
                }
                xQueueReset(s_queue);  // події, що прийшли під час зняття, вже не актуальні
                s_present.store(false);
                s_installed.store(false);
                setState(State::Off);
                s_endOk.store(true);
                s_mgrTask = nullptr;
                s_life.store(Life::Off);  // УСЕ, що стосується стеку, вже знято; далі лише self-delete
                xSemaphoreGive(s_endDone);
                vTaskDelete(nullptr);  // чисте завершення: звільнення стеку й TCB — у idle-задачі
                return;
            }
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
        case State::Off: return "Off";
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
    ensureObjects();
    if (xSemaphoreTake(s_lifeMutex, pdMS_TO_TICKS(usb_host_cfg::kLifeLockWaitMs)) != pdTRUE) {
        Serial.printf("%s begin: lifecycle busy\n", kTag);
        return false;
    }
    bool result = false;
    const Life life = s_life.load();
    if (life == Life::Up) {
        result = s_installed.load();  // уже піднято: нешкідливо
    } else if (life != Life::Off) {
        Serial.printf("%s begin: busy (life=%s)\n", kTag, lifeName(life));
    } else {
        // Чистий старт: задач немає, тож безпечно скинути все, що могли лишити попередні цикли.
        xQueueReset(s_queue);
        drain(s_installDone);
        drain(s_endDone);
        drain(s_allFree);
        drain(s_libExited);
        s_libStop.store(false);
        s_abortIo.store(false);
        s_present.store(false);
        s_installed.store(false);
        s_life.store(Life::Starting);

        const BaseType_t ok = xTaskCreatePinnedToCore(mgrTask, "usb_mgr", usb_host_cfg::kMgrTaskStack, nullptr,
                                                      usb_host_cfg::kMgrTaskPriority, &s_mgrTask,
                                                      usb_host_cfg::kTaskCore);
        if (ok != pdPASS) {
            Serial.printf("%s usb_mgr task create failed\n", kTag);
            s_mgrTask = nullptr;
            setError(ESP_ERR_NO_MEM, "mgr_task");
            setState(State::Error);
            s_life.store(Life::Off);
        } else if (xSemaphoreTake(s_installDone, pdMS_TO_TICKS(usb_host_cfg::kInstallWaitMs)) != pdTRUE) {
            Serial.printf("%s install wait timeout (%u ms); installation continues in background\n", kTag,
                          (unsigned)usb_host_cfg::kInstallWaitMs);
        } else {
            result = s_installed.load();
        }
    }
    xSemaphoreGive(s_lifeMutex);
    return result;
}

bool end() {
    ensureObjects();
    if (s_mgrTask != nullptr && xTaskGetCurrentTaskHandle() == s_mgrTask) {
        Serial.printf("%s end: must not be called from usb_mgr (event callback)\n", kTag);
        return false;
    }
    if (xSemaphoreTake(s_lifeMutex, pdMS_TO_TICKS(usb_host_cfg::kLifeLockWaitMs)) != pdTRUE) {
        Serial.printf("%s end: lifecycle busy\n", kTag);
        return false;
    }
    bool ok = true;
    bool stopped = false;
    const Life life = s_life.load();
    if (life != Life::Off) {
        bool waiting = true;
        if (life != Life::Stopping) {  // Stopping: попередній end() за таймаутом — просто чекаємо далі
            drain(s_endDone);
            s_endOk.store(true);
            s_life.store(Life::Stopping);
            Msg m = {};
            m.type = MsgType::CmdEnd;
            if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
                Serial.printf("%s end: queue full\n", kTag);
                s_life.store(life);
                waiting = false;
                ok = false;
            }
        }
        if (waiting) {
            if (xSemaphoreTake(s_endDone, pdMS_TO_TICKS(usb_host_cfg::kEndWaitMs)) != pdTRUE) {
                Serial.printf("%s end: timeout (%u ms), stopping continues in background\n", kTag,
                              (unsigned)usb_host_cfg::kEndWaitMs);
                ok = false;
            } else {
                ok = s_endOk.load() && s_life.load() == Life::Off;
                stopped = ok;
            }
        }
    }
    xSemaphoreGive(s_lifeMutex);
    if (stopped) {
        // Дати idle-задачі звільнити стек і TCB usb_mgr (vTaskDelete(nullptr)), інакше free занижений.
        vTaskDelay(pdMS_TO_TICKS(usb_host_cfg::kEndSettleMs));
    }
    return ok;
}

bool isStarted() {
    const Life l = s_life.load();
    return l == Life::Up || l == Life::Starting;
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
    Serial.printf("%s stat: life=%s installed=%d host_up=%d msc_up=%d state=%s present=%d path=%s\n", kTag,
                  lifeName(s_life.load()), (int)s_installed.load(), (int)s_hostUp.load(), (int)s_mscUp.load(),
                  stateName(s_state.load()), (int)s_present.load(), mountPath());
    Serial.printf("%s stat: last_error=0x%x (%s) at '%s' dropped_events=%u end_ok=%d tasks=%u\n", kTag,
                  (unsigned)e.code, errorName(e.code), e.where, (unsigned)s_dropped.load(),
                  (int)s_endOk.load(), (unsigned)uxTaskGetNumberOfTasks());
    // IDF: uxTaskGetStackHighWaterMark повертає БАЙТИ (мінімум вільного стеку за час роботи).
    const TaskHandle_t mgr = s_mgrTask;
    const TaskHandle_t lib = s_libTask;
    if (mgr != nullptr) {
        Serial.printf("%s stat: usb_mgr stack min free=%u of %u bytes\n", kTag,
                      (unsigned)uxTaskGetStackHighWaterMark(mgr), (unsigned)usb_host_cfg::kMgrTaskStack);
    }
    if (lib != nullptr) {
        Serial.printf("%s stat: usb_lib stack min free=%u of %u bytes\n", kTag,
                      (unsigned)uxTaskGetStackHighWaterMark(lib), (unsigned)usb_host_cfg::kLibTaskStack);
    }
}

void cmdLs(const char* arg) {
    if (!isMounted() || s_ioMutex == nullptr) {
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
    if (!s_installed.load() || s_life.load() != Life::Up) {
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

// [USB3] usb.life N: N циклів begin -> (якщо є флешка: чекати Mounted) -> end. Блокує викликача (loopTask).
// Після кожного циклу друкує free / largest / min_free внутрішньої RAM і різницю з виміром ДО першого begin.
void cmdLife(const char* arg) {
    const long n = (arg != nullptr) ? strtol(arg, nullptr, 10) : 0;
    if (n < 1 || (unsigned long)n > usb_host_cfg::kCycleMaxCount) {
        Serial.printf("%s life: usage usb.life N (1..%u)\n", kTag, (unsigned)usb_host_cfg::kCycleMaxCount);
        return;
    }
    if (s_life.load() != Life::Off) {
        Serial.printf("%s life: need life=Off (run usb.end first), now %s\n", kTag, lifeName(s_life.load()));
        return;
    }
    const long base_free = (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const long base_largest = (long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const long base_min = (long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const unsigned base_tasks = (unsigned)uxTaskGetNumberOfTasks();
    Serial.printf("%s life: baseline (before first begin) free=%ld largest=%ld min_free=%ld tasks=%u cycles=%ld\n",
                  kTag, base_free, base_largest, base_min, base_tasks, n);

    long maxAbsFree = 0;
    long maxAbsLargest = 0;
    long minFreeAfterFirst = 0;
    long minFreeLast = 0;
    long nonZero = 0;
    long done = 0;
    for (long i = 1; i <= n; ++i) {
        const bool okBegin = begin();
        if (!okBegin) {
            Serial.printf("%s life %ld/%ld: begin FAILED (last error 0x%x at '%s')\n", kTag, i, n,
                          (unsigned)lastError().code, lastError().where);
            end();  // прибрати те, що встигло піднятись
            break;
        }
        const uint32_t tp = millis();
        while (!s_present.load() && (millis() - tp) < usb_host_cfg::kLifePresentWaitMs) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        const bool had = s_present.load();
        bool mounted = false;
        uint32_t mountMs = 0;
        if (had) {
            const uint32_t tm = millis();
            while ((millis() - tm) < usb_host_cfg::kLifeMountWaitMs) {
                const State st = state();
                if (st == State::Mounted) {
                    mounted = true;
                    break;
                }
                if (st == State::Error || !s_present.load()) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            mountMs = millis() - tm;
        }
        const unsigned upFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const unsigned upLargest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

        const uint32_t te = millis();
        const bool okEnd = end();
        const uint32_t endMs = millis() - te;
        const long fr = (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const long lg = (long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const long mn = (long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        Serial.printf("%s life %ld/%ld: device=%d mounted=%d mount_wait=%u ms | up free=%u largest=%u | end=%s in %u ms "
                      "| after free=%ld largest=%ld min_free=%ld | d_free=%ld d_largest=%ld d_min=%ld tasks=%u\n",
                      kTag, i, n, (int)had, (int)mounted, (unsigned)mountMs, upFree, upLargest,
                      okEnd ? "ok" : "FAILED", (unsigned)endMs, fr, lg, mn, fr - base_free, lg - base_largest,
                      mn - base_min, (unsigned)uxTaskGetNumberOfTasks());
        if (!okEnd) {
            Serial.printf("%s life: end FAILED (last error 0x%x at '%s'), stopping\n", kTag,
                          (unsigned)lastError().code, lastError().where);
            break;
        }
        ++done;
        const long dF = fr - base_free;
        const long dL = lg - base_largest;
        if (labs(dF) > maxAbsFree) maxAbsFree = labs(dF);
        if (labs(dL) > maxAbsLargest) maxAbsLargest = labs(dL);
        if (dF != 0 || dL != 0) ++nonZero;
        if (i == 1) minFreeAfterFirst = mn;
        minFreeLast = mn;
    }
    if (done > 0) {
        Serial.printf("%s life done: %ld/%ld cycles, max|d_free|=%ld max|d_largest|=%ld cycles_with_diff=%ld "
                      "min_free after cycle 1=%ld, after last=%ld (change %ld)\n",
                      kTag, done, n, maxAbsFree, maxAbsLargest, nonZero, minFreeAfterFirst, minFreeLast,
                      minFreeLast - minFreeAfterFirst);
    } else {
        Serial.printf("%s life done: 0 cycles completed\n", kTag);
    }
}

void cmdEnd() {
    const uint32_t t0 = millis();
    const bool ok = end();
    Serial.printf("%s end: %s in %u ms (state=%s life=%s)\n", kTag, ok ? "ok" : "failed (see usb.stat)",
                  (unsigned)(millis() - t0), stateName(s_state.load()), lifeName(s_life.load()));
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
    } else if (isCmd(line, "usb.life")) {
        cmdLife(skipSpaces(line + strlen("usb.life")));
    } else if (isCmd(line, "usb.begin")) {
        const bool ok = begin();
        Serial.printf("%s begin: %s\n", kTag, ok ? "ok" : "failed (see usb.stat)");
    } else if (isCmd(line, "usb.end")) {
        cmdEnd();
    } else if (isCmd(line, "usb.help")) {
        Serial.printf("%s commands: usb.stat | usb.ls [path] | usb.cycle N | usb.life N | usb.heap | usb.begin | usb.end\n",
                      kTag);
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
