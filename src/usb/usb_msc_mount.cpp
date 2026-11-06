// [USB13] DEF-9: unmountLocked() ПЕРЕД msc_host_vfs_unregister закриває всі відкриті файли / каталоги обгортки
// (usb_fs::closeAllForUnmountLocked); `usb.heap` розширено (класи пам'яті, невдалі виділення, дескриптори).
// [USB2] Постійний модуль монтування USB-флешки (USB-host MSC). Замінює src/usb_host_test.*.
// [USB7] ДОДАНО лише диспетчеризацію консольних команд відтворення (usb.play / usb.pstat / usb.seek / usb.stopf /
// usb.ended -> usb/usb_play_diag.*) і рядок usb.help; логіка монтування / зняття без змін.
// [USB5] Безпечне зняття стека: end() НЕ доходить до usb_host_uninstall(), поки usb_lib не «тиха» (немає подій і
// обробник підключення не триває) і num_devices != 0; перед зупинкою usb_lib чекаємо ALL_FREE + друге тихе вікно;
// перед КОЖНОЮ спробою usb_host_uninstall() перевіряємо num_devices == 0. Якщо безпеку не встановлено — стек
// лишається піднятим, Error + needsReboot() (без аборту IDF). Діагностика: usb.sweep, лог етапів зняття.
// [USB4] Виправлено дефекти begin() / стану Error (begin() true лише при працюючому MSC; стан «стек пошкоджено»
// після невдалого end() — s_needReboot, begin() відмовляє) + діагностика (USB_HOST_DIAG): мітки часу етапів begin(),
// проба `usb.begin`, `usb.prio`, `usb.trace`, розширений дамп збою end(), відлуння консолі. Дефолтні
// пріоритети й таймаути НЕ змінено.
// [USB6] ДОДАНО блокування тому (volumeLock / volumeUnlock) і покоління монтування для fs::FS-обгортки
// (src/usb/usb_fs.*); стан s_volReady / s_volGen змінюється під s_ioMutex у unmountLocked().
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
#include "usb/usb_fs.h"  // [USB6] usb.fs.*; [USB13] closeAllForUnmountLocked() потрібен і в релізі (DIAG = 0)
#if USB_HOST_DIAG
#include "usb/usb_play_diag.h"  // [USB7] usb.play / usb.pstat / usb.seek / usb.stopf / usb.ended
#include "usb/usb_memdiag.h"     // [USB13] usb.heap: класи пам'яті, невдалі виділення
#endif
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
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
std::atomic<bool> s_needReboot{false};  // [USB4] MSC знято, а USB-host лишився піднятим (збій end()): begin() відмовляє
// [USB6] Стан для fs::FS-обгортки. s_volReady = true від успішного монтування до початку розмонтування
// (змінюється лише під s_ioMutex); s_volGen змінюється при кожному монтуванні й розмонтуванні.
std::atomic<bool> s_volReady{false};
std::atomic<uint32_t> s_volGen{1};
// [USB5] Активність usb_lib: виклик usb_host_lib_handle_events() або чекає подію (≤ kLibWaitMs), або ОБРОБЛЯЄ її
// (обробник підключення, перелічення). Другий випадок видно як дуже довгий виклик або як повернення ESP_OK.
std::atomic<uint32_t> s_libCallStartMs{0};  // millis() початку поточного виклику handle_events
std::atomic<bool> s_libInCall{false};       // usb_lib зараз усередині handle_events
std::atomic<uint32_t> s_libActiveMs{0};     // millis() останнього «активного» повернення (ESP_OK або довгий виклик)
#if USB_HOST_DIAG
std::atomic<uint32_t> s_libGapMaxMs{0};     // найбільший проміжок між двома активними поверненнями (для kEndQuietMs)
#endif

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

// [USB4] Ефективні пріоритети задач USB. У релізі (USB_HOST_DIAG = 0) — константи з usb_host_config.h;
// при USB_HOST_DIAG = 1 їх можна змінити командою `usb.prio` для НАСТУПНОГО begin() (дефолти не чіпаємо).
#if USB_HOST_DIAG
std::atomic<unsigned> s_prioLib{usb_host_cfg::kLibTaskPriority};
std::atomic<unsigned> s_prioMsc{usb_host_cfg::kMscTaskPriority};
std::atomic<unsigned> s_prioMgr{usb_host_cfg::kMgrTaskPriority};
unsigned libPrio() { return s_prioLib.load(); }
unsigned mscPrio() { return s_prioMsc.load(); }
unsigned mgrPrio() { return s_prioMgr.load(); }
#else
constexpr unsigned libPrio() { return usb_host_cfg::kLibTaskPriority; }
constexpr unsigned mscPrio() { return usb_host_cfg::kMscTaskPriority; }
constexpr unsigned mgrPrio() { return usb_host_cfg::kMgrTaskPriority; }
#endif

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

#if USB_HOST_DIAG
// [USB4] Мітки часу етапів begin() (мс від виклику begin(), що почав чистий старт) і знімки внутрішньої RAM.
// Порядок «конвеєра»: host_installed -> msc_installed -> ready -> enum_seen -> connected -> dev_installed -> mounted.
// lib_wake (перше повернення usb_host_lib_handle_events у usb_lib) і begin_returned — окремо.
// «Перша подія клієнта» НЕ спостережувана окремо без правки lib/usb_host_msc (заборонено): її заміняє connected
// (колбек MSC після NEW_DEV + перевірки класу пристрою); enum_seen = usb_host_lib_info().num_devices > 0 (проба).
constexpr uint32_t kNotReached = 0xFFFFFFFFu;
enum Stage : uint8_t {
    StHostInstalled = 0,
    StMscInstalled,
    StReady,
    StEnumSeen,
    StConnected,
    StDevInstalled,
    StMounted,
    StLibWake,
    StBeginReturned,
    StCount
};
constexpr uint8_t kPipelineCount = StMounted + 1;
const char* const kStageNames[StCount] = {"host_installed", "msc_installed", "ready",      "enum_seen",
                                          "connected",      "dev_installed", "mounted",    "lib_wake",
                                          "begin_returned"};

std::atomic<uint32_t> s_t0{0};
std::atomic<bool> s_traceValid{false};
std::atomic<uint32_t> s_stageMs[StCount];
uint32_t s_stageFree[StCount];
uint32_t s_stageLargest[StCount];
uint32_t s_stageMin[StCount];
std::atomic<uint32_t> s_libEvents{0};   // кількість повернень usb_host_lib_handle_events з ESP_OK
std::atomic<uint32_t> s_libFlagsOr{0};  // OR усіх прапорців подій бібліотеки (сирі біти)

void traceReset() {
    for (uint8_t i = 0; i < StCount; ++i) {
        s_stageMs[i].store(kNotReached);
        s_stageFree[i] = 0;
        s_stageLargest[i] = 0;
        s_stageMin[i] = 0;
    }
    s_libEvents.store(0);
    s_libFlagsOr.store(0);
    s_t0.store(millis());
    s_traceValid.store(true);
}

void traceHeap(Stage s) {
    s_stageFree[s] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_stageLargest[s] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_stageMin[s] = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}

// Фіксує ПЕРШЕ досягнення етапу. heap = true: знімок RAM (лише з контекстів задач, не з колбека MSC).
void traceMark(Stage s, bool heap) {
    if (!s_traceValid.load()) {
        return;
    }
    uint32_t expected = kNotReached;
    const uint32_t dt = millis() - s_t0.load();
    if (s_stageMs[s].compare_exchange_strong(expected, dt) && heap) {
        traceHeap(s);
    }
}

// Етап уже позначено з колбека (без RAM) — дописати знімок RAM (трохи пізніше за мітку часу).
void traceHeapIfMissing(Stage s) {
    if (s_traceValid.load() && s_stageMs[s].load() != kNotReached && s_stageFree[s] == 0) {
        traceHeap(s);
    }
}

const char* furthestStage() {
    const char* last = "none";
    for (uint8_t i = 0; i < kPipelineCount; ++i) {
        if (s_stageMs[i].load() != kNotReached) {
            last = kStageNames[i];
        }
    }
    return last;
}

void printTaskInfo(const char* name, TaskHandle_t h, uint32_t stackTotal) {
    if (h == nullptr) {
        Serial.printf("%s trace: task %-8s not running\n", kTag, name);
        return;
    }
    // IDF: uxTaskGetStackHighWaterMark повертає БАЙТИ (мінімум вільного стеку за час роботи).
    Serial.printf("%s trace: task %-8s prio=%u stack min free=%u of %u bytes\n", kTag, name,
                  (unsigned)uxTaskPriorityGet(h), (unsigned)uxTaskGetStackHighWaterMark(h), (unsigned)stackTotal);
}

void printLibInfo(const char* label) {
    usb_host_lib_info_t info = {};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_hostUp.load()) {
        err = usb_host_lib_info(&info);
    }
    Serial.printf("%s %s: lib_info=%s num_devices=%d num_clients=%d lib_events=%u lib_flags_or=0x%x present=%d "
                  "state=%s need_reboot=%d lib_idle_ms=%u lib_gap_max_ms=%u\n",
                  kTag, label, errorName(err), info.num_devices, info.num_clients, (unsigned)s_libEvents.load(),
                  (unsigned)s_libFlagsOr.load(), (int)s_present.load(), stateName(s_state.load()),
                  (int)s_needReboot.load(), (unsigned)(millis() - s_libActiveMs.load()),
                  (unsigned)s_libGapMaxMs.load());
}

void printStageReport(const char* why) {
    if (!s_traceValid.load()) {
        Serial.printf("%s trace: no begin() trace yet\n", kTag);
        return;
    }
    Serial.printf("%s trace: %s (ms since begin() call; heap = internal free/largest/min_free at the stage)\n", kTag,
                  why);
    for (uint8_t i = 0; i < StCount; ++i) {
        const uint32_t v = s_stageMs[i].load();
        if (v == kNotReached) {
            Serial.printf("%s trace   %-15s not reached\n", kTag, kStageNames[i]);
        } else if (s_stageFree[i] == 0) {
            Serial.printf("%s trace   %-15s %6u ms  heap=-\n", kTag, kStageNames[i], (unsigned)v);
        } else {
            Serial.printf("%s trace   %-15s %6u ms  free=%u largest=%u min_free=%u\n", kTag, kStageNames[i],
                          (unsigned)v, (unsigned)s_stageFree[i], (unsigned)s_stageLargest[i],
                          (unsigned)s_stageMin[i]);
        }
    }
    Serial.printf("%s trace: furthest pipeline stage=%s\n", kTag, furthestStage());
    printLibInfo("trace");
    if (s_life.load() != Life::Off && s_mgrTask != nullptr) {
        printTaskInfo("usb_mgr", s_mgrTask, usb_host_cfg::kMgrTaskStack);
    }
    if (s_libTask != nullptr) {
        printTaskInfo("usb_lib", s_libTask, usb_host_cfg::kLibTaskStack);
    }
    // Фонова задача драйвера MSC створена всередині msc_host_install() з іменем "USB MSC" (msc_host.c), без
    // повернення handle: шукаємо за іменем.
    if (s_mscUp.load()) {
        printTaskInfo("USB MSC", xTaskGetHandle("USB MSC"), usb_host_cfg::kMscTaskStack);
    }
    printHeap("trace");
}

// Дамп при збої зняття (all_free_timeout): те, що можна дізнатись лише з API, наявних у збірці.
// Список адрес пристроїв, що лишились, НЕ друкуємо: потрібний API з usb/usb_host.h не прикладено (див. звіт).
void dumpEndFailure() {
    Serial.printf("%s end-failure dump: s_dev=%p s_vfs=%p msc_up=%d host_up=%d lib_task=%d mgr_task=%d\n", kTag,
                  (void*)s_dev, (void*)s_vfs, (int)s_mscUp.load(), (int)s_hostUp.load(), (int)(s_libTask != nullptr),
                  (int)(s_mgrTask != nullptr));
    printLibInfo("end-failure");
    Serial.printf("%s end-failure dump: USB MSC task %s\n", kTag,
                  xTaskGetHandle("USB MSC") != nullptr ? "STILL EXISTS" : "gone");
    if (s_libTask != nullptr) {
        printTaskInfo("usb_lib", s_libTask, usb_host_cfg::kLibTaskStack);
    }
    printHeap("end-failure");
}

// [USB5] Лог етапів зняття: t = мс від початку teardownStack(); після кожного рядка Serial.flush(), щоб при аборті
// останній рядок не загубився. Лише друк, поведінки не змінює.
uint32_t s_endT0 = 0;
void endStage(const char* stage, esp_err_t err) {
    int nd = -1, nc = -1;
    if (s_hostUp.load()) {
        usb_host_lib_info_t info = {};
        if (usb_host_lib_info(&info) == ESP_OK) {
            nd = info.num_devices;
            nc = info.num_clients;
        }
    }
    const uint32_t now = millis();
    const bool busy = s_libInCall.load() && (now - s_libCallStartMs.load()) > (usb_host_cfg::kLibWaitMs + usb_host_cfg::kEndPollMs);
    Serial.printf("%s endlog t=%u stage=%s res=%s num_devices=%d num_clients=%d lib_idle_ms=%u lib_busy=%d "
                  "gap_max_ms=%u\n",
                  kTag, (unsigned)(now - s_endT0), stage, errorName(err), nd, nc,
                  (unsigned)(now - s_libActiveMs.load()), (int)busy, (unsigned)s_libGapMaxMs.load());
    Serial.flush();
}
#define END_STAGE(stage, err) endStage((stage), (err))
#else
#define END_STAGE(stage, err) ((void)0)
#endif  // USB_HOST_DIAG

// [USB4] Мітки етапів: у релізі (USB_HOST_DIAG = 0) компілюються геть.
#if USB_HOST_DIAG
#define USB_TRACE(stage, heap) traceMark(stage, heap)
#define USB_TRACE_HEAP(stage) traceHeapIfMissing(stage)
#else
#define USB_TRACE(stage, heap) ((void)0)
#define USB_TRACE_HEAP(stage) ((void)0)
#endif

// --- Колбек драйвера MSC (задача драйвера; без блокувань і без роботи з FS) -------------------
void mscEventCb(const msc_host_event_t* event, void* arg) {
    (void)arg;
    Msg m = {};
    if (event->event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
        m.type = MsgType::Connected;
        m.addr = event->device.address;
        s_present.store(true);
        USB_TRACE(StConnected, false);  // [USB4] лише мітка часу (без RAM: колбек не блокуємо)
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
        const uint32_t tCall = millis();
        s_libCallStartMs.store(tCall);
        s_libInCall.store(true);
        const esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(usb_host_cfg::kLibWaitMs), &flags);
        s_libInCall.store(false);
        const uint32_t tNow = millis();
        // [USB5] «Активне» повернення: події оброблено (ESP_OK) або виклик тривав довше за очікування події.
        if (err == ESP_OK || (tNow - tCall) > (usb_host_cfg::kLibWaitMs + usb_host_cfg::kEndPollMs)) {
#if USB_HOST_DIAG
            if (err == ESP_OK) {
                const uint32_t gap = tNow - s_libActiveMs.load();
                if (gap > s_libGapMaxMs.load()) {
                    s_libGapMaxMs.store(gap);
                }
            }
#endif
            s_libActiveMs.store(tNow);
        }
        USB_TRACE(StLibWake, true);  // [USB4] перше повернення з очікування подій бібліотеки (будь-який код)
        if (err == ESP_OK) {
#if USB_HOST_DIAG
            s_libEvents.fetch_add(1);
            s_libFlagsOr.fetch_or(flags);
#endif
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
    USB_TRACE(StDevInstalled, true);  // [USB4]

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
    s_volGen.fetch_add(1);   // [USB6]
    s_volReady.store(true);  // [USB6]
    return ESP_OK;
}

// Порядок: спершу VFS, потім пристрій. Тримає s_ioMutex: чекає завершення usb.ls (яке перериває s_abortIo).
void unmountLocked() {
    xSemaphoreTake(s_ioMutex, portMAX_DELAY);
    const uint32_t closingGen = s_volReady.load() ? s_volGen.load() : 0;  // [USB13] монтування, що знімається
    s_volReady.store(false);  // [USB6] до розмонтування: обгортка більше не звертається до VFS
    s_volGen.fetch_add(1);    // [USB6]
    // [USB13] DEF-9: закриваємо ВСІ відкриті файли / каталоги, поки VFS ще зареєстровано, а FatFs змонтовано
    // (для читання f_close не звертається до диска, тож це безпечно й після вийняття носія). Інакше fclose() пізніше,
    // на знятому томі, неможливий, а FILE + його буфер + буфер сектора FatFs (fp->buf) лишаються до перезавантаження.
    {
        const usb_fs::UnmountClosed uc = usb_fs::closeAllForUnmountLocked(closingGen);
        if (uc.files + uc.dirs + uc.deferred + uc.leaked != 0) {
            Serial.printf("%s unmount: closed %u file(s), %u dir(s), %u deferred; leaked=%u (before vfs_unregister)\n",
                          kTag, (unsigned)uc.files, (unsigned)uc.dirs, (unsigned)uc.deferred, (unsigned)uc.leaked);
        }
    }
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
            USB_TRACE(StMounted, true);  // [USB4]
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
    USB_TRACE_HEAP(StConnected);  // [USB4] знімок RAM для етапу connected (трохи пізніше за мітку часу колбека)
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
// Створює задачу usb_lib. [USB5] Винесено з installStack(): те саме потрібне, щоб відновити usb_lib, якщо зняття
// довелось перервати після її зупинки (під час зупинки зʼявився пристрій).
esp_err_t startLibTask() {
    s_libStop.store(false);
    drain(s_libExited);
    drain(s_allFree);
    s_libActiveMs.store(millis());  // тихе вікно відлічується від запуску usb_lib
    s_libInCall.store(false);
    const BaseType_t ok = xTaskCreatePinnedToCore(libTask, "usb_lib", usb_host_cfg::kLibTaskStack, nullptr,
                                                  libPrio(), &s_libTask, usb_host_cfg::kTaskCore);
    if (ok != pdPASS) {
        Serial.printf("%s usb_lib task create failed\n", kTag);
        s_libTask = nullptr;
        setError(ESP_ERR_NO_MEM, "usb_lib_task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
    USB_TRACE(StHostInstalled, true);  // [USB4]

#if USB_HOST_DIAG
    s_libGapMaxMs.store(0);
#endif
    err = startLibTask();
    if (err != ESP_OK) {
        return err;
    }

    msc_host_driver_config_t mscCfg = {};
    mscCfg.create_backround_task = true;  // (sic) так названо поле в msc_host.h
    mscCfg.task_priority = mscPrio();
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
    USB_TRACE(StMscInstalled, true);  // [USB4]

    // [USB4] Перевірка, що драйвер MSC справді працює: його клієнт зареєстровано в USB-host
    // (msc_host_install() викликає usb_host_client_register — msc_host.c). Критерій «num_clients >= 1» спирається
    // на опис usb_host_lib_info_t (usb/usb_host.h НЕ прикладено до кроку): якщо тут буде false-негатив —
    // M1 покаже num_clients = 0 при працюючому MSC, тоді перевірку треба послабити.
    usb_host_lib_info_t info = {};
    const esp_err_t ierr = usb_host_lib_info(&info);
    Serial.printf("%s install check: usb_host_lib_info %s num_devices=%d num_clients=%d\n", kTag, errorName(ierr),
                  info.num_devices, info.num_clients);
    if (ierr == ESP_OK && info.num_clients < 1) {
        Serial.printf("%s install check FAILED: MSC client is not registered\n", kTag);
        setError(ESP_ERR_INVALID_STATE, "msc_no_client");
        return ESP_ERR_INVALID_STATE;
    }
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

// [USB4] Очікування події ALL_FREE після usb_host_device_free_all() (сумарно kEndStepWaitMs, як і раніше).
// При USB_HOST_DIAG очікування ділиться на кроки kEndDiagSliceMs з друком num_devices / num_clients:
// видно, чи лишок пристроїв коли-небудь зменшується. Загальний час очікування не змінюється.
bool waitAllFree() {
#if USB_HOST_DIAG
    const uint32_t t0 = millis();
    for (;;) {
        const uint32_t waited = millis() - t0;
        if (waited >= usb_host_cfg::kEndStepWaitMs) {
            return false;
        }
        uint32_t slice = usb_host_cfg::kEndStepWaitMs - waited;
        if (slice > usb_host_cfg::kEndDiagSliceMs) {
            slice = usb_host_cfg::kEndDiagSliceMs;
        }
        if (xSemaphoreTake(s_allFree, pdMS_TO_TICKS(slice)) == pdTRUE) {
            Serial.printf("%s ALL_FREE after %u ms\n", kTag, (unsigned)(millis() - t0));
            return true;
        }
        char label[24];
        snprintf(label, sizeof(label), "wait_all_free %u ms", (unsigned)(millis() - t0));
        printLibInfo(label);
    }
#else
    return xSemaphoreTake(s_allFree, pdMS_TO_TICKS(usb_host_cfg::kEndStepWaitMs)) == pdTRUE;
#endif
}

// [USB5] true, якщо usb_lib зараз усередині тривалого виклику usb_host_lib_handle_events() (обробляє подію:
// підключення / перелічення). Звичайне очікування події триває ≤ kLibWaitMs, тож виклик довший за
// kLibWaitMs + kEndPollMs — це обробка.
bool libBusyNow() {
    return s_libInCall.load() &&
           (millis() - s_libCallStartMs.load()) > (usb_host_cfg::kLibWaitMs + usb_host_cfg::kEndPollMs);
}

// [USB5] Чекає, поки usb_lib буде «тихою» протягом usb_host_cfg::kEndQuietMs: за цей час жодного активного
// повернення handle_events і жодного спостереження «обробка триває» (опитуємо кожні kEndPollMs, тож тривалий
// виклик помічаємо не пізніше ніж через kLibWaitMs + kEndPollMs). Вікно відлічується від початку спостереження.
// false — не вдалось за kEndStepWaitMs (перелічення / обробка не завершується).
// ОБМЕЖЕННЯ: пристрій, що підключиться (інтерфейс USB) саме наприкінці вікна, цим не виявляється.
bool waitLibQuiet(const char* label) {
    const uint32_t t0 = millis();
    uint32_t lastChange = t0;
    uint32_t seenActive = s_libActiveMs.load();
    for (;;) {
        const uint32_t now = millis();
        if (libBusyNow()) {
            lastChange = now;
        }
        const uint32_t a = s_libActiveMs.load();
        if (a != seenActive) {
            seenActive = a;
            lastChange = now;
        }
        if ((now - lastChange) >= usb_host_cfg::kEndQuietMs) {
            Serial.printf("%s quiet(%s): ok after %u ms\n", kTag, label, (unsigned)(now - t0));
            return true;
        }
        if ((now - t0) >= usb_host_cfg::kEndStepWaitMs) {
            Serial.printf("%s quiet(%s): NOT reached in %u ms (lib busy=%d, idle=%u ms)\n", kTag, label,
                          (unsigned)usb_host_cfg::kEndStepWaitMs, (int)libBusyNow(),
                          (unsigned)(now - s_libActiveMs.load()));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(usb_host_cfg::kEndPollMs));
    }
}

// [USB5] Кількість пристроїв за usb_host_lib_info(); -1, якщо дізнатись не вдалось.
int libDevices() {
    usb_host_lib_info_t info = {};
    if (usb_host_lib_info(&info) != ESP_OK) {
        return -1;
    }
    return info.num_devices;
}

// [USB5] Зняття стеку за стадіями (прапорці s_mscUp / s_hostUp / s_libTask): ідемпотентне, повторний виклик після
// помилки продовжує з місця зупинки. Викликається лише з usb_mgr, після unmountLocked().
// Порядок (обґрунтування — IDF v5.5.2, див. звіт USB5):
//   0. тихе вікно usb_lib (ДО msc_host_uninstall: якщо не тихо — стек лишається повністю справним, end() можна
//      повторити);
//   1. msc_host_uninstall;
//   2. usb_host_device_free_all + ALL_FREE (usb_lib ЖИВА: пристрій і його EP0-пайп звільняються в задачі usb_lib);
//      після ALL_FREE — друге тихе вікно;
//   3. num_devices == 0, лише тоді зупинка usb_lib;
//   4. num_devices == 0 перед КОЖНОЮ спробою usb_host_uninstall (його внутрішні шари — ESP_ERROR_CHECK: помилка
//      = аборт, тож викликати лише коли безпечно); спроб ≤ 1 + kEndUninstallRetries.
// usb_host_lib_set_root_port_power(false) НАВМИСНЕ не викликається окремо (див. звіт: гілка CONNECTION безумовно
// ставить ENABLED, повторний POWER_OFF у usb_host_uninstall дав би assert; а POWER_OFF при живій usb_lib без
// вузла пристрою веде до dev_tree_node_dev_gone — його тіла у дампі немає).
esp_err_t teardownStack() {
    esp_err_t err = ESP_OK;
#if USB_HOST_DIAG
    s_endT0 = millis();
#endif
    END_STAGE("begin", ESP_OK);

    if (s_hostUp.load() && s_libTask != nullptr) {
        if (!waitLibQuiet("pre")) {
            setError(ESP_ERR_TIMEOUT, "end_not_quiet");
            END_STAGE("quiet_pre", ESP_ERR_TIMEOUT);
            return ESP_ERR_TIMEOUT;  // MSC ще піднято: стек справний, end() можна повторити
        }
        END_STAGE("quiet_pre", ESP_OK);
    }

    if (s_mscUp.load()) {
        err = msc_host_uninstall();  // INVALID_STATE: драйвер не встановлено або пристрій ще відкрито
        Serial.printf("%s msc_host_uninstall: %s (0x%x)\n", kTag, errorName(err), (unsigned)err);
        END_STAGE("msc_uninstall", err);
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
            END_STAGE("free_all", err);
            if (err == ESP_ERR_NOT_FINISHED) {
                if (!waitAllFree()) {
                    Serial.printf("%s ALL_FREE not received in %u ms\n", kTag,
                                  (unsigned)usb_host_cfg::kEndStepWaitMs);
                    setError(ESP_ERR_TIMEOUT, "all_free_timeout");
#if USB_HOST_DIAG
                    dumpEndFailure();  // [USB4] лише друк, поведінку не змінює
#endif
                    return ESP_ERR_TIMEOUT;
                }
                Serial.printf("%s USB_HOST_LIB_EVENT_FLAGS_ALL_FREE received\n", kTag);
                END_STAGE("all_free", ESP_OK);
                // [USB5] Друге тихе вікно: звільнення пристрою й пайпа виконує usb_lib (usbh_process) — дати їй
                // закінчити, а не зупиняти посеред обробки.
                if (!waitLibQuiet("post_free")) {
                    setError(ESP_ERR_TIMEOUT, "end_not_quiet_post");
                    END_STAGE("quiet_post", ESP_ERR_TIMEOUT);
                    return ESP_ERR_TIMEOUT;
                }
                END_STAGE("quiet_post", ESP_OK);
            } else if (err != ESP_OK) {
                setError(err, "device_free_all");
                return err;
            }

            const int devs = libDevices();
            if (devs != 0) {
                Serial.printf("%s teardown stopped: num_devices=%d after free_all (not safe to stop usb_lib)\n",
                              kTag, devs);
                setError(ESP_ERR_INVALID_STATE, "devices_left");
                END_STAGE("devices_check", ESP_ERR_INVALID_STATE);
                return ESP_ERR_INVALID_STATE;  // usb_lib ЛИШАЄТЬСЯ живою
            }
            END_STAGE("devices_check", ESP_OK);

            err = stopLibTask();
            END_STAGE("lib_stop", err);
            if (err != ESP_OK) {
                return err;
            }
        }

        // usb_lib зупинено (або вже була): події бібліотеки на час повторів обробляє usb_mgr (єдиний виклик
        // handle_events). Перед КОЖНОЮ спробою — перевірка num_devices == 0 (інакше hcd_port_deinit → аборт).
        uint32_t retries = 0;
        for (;;) {
            const int devs = libDevices();
            if (devs != 0) {
                Serial.printf("%s teardown stopped before usb_host_uninstall: num_devices=%d (retry %u)\n", kTag,
                              devs, (unsigned)retries);
                setError(ESP_ERR_INVALID_STATE, "devices_before_uninstall");
                END_STAGE("uninstall_guard", ESP_ERR_INVALID_STATE);
                // Повернути usb_lib, щоб пристрій, що зʼявився, могли обробити (стек лишається піднятим).
                (void)startLibTask();
                return ESP_ERR_INVALID_STATE;
            }
            END_STAGE("uninstall_try", ESP_OK);
            err = usb_host_uninstall();
            END_STAGE("uninstall", err);
            if (err != ESP_ERR_INVALID_STATE || retries >= usb_host_cfg::kEndUninstallRetries) {
                break;
            }
            ++retries;
            uint32_t flags = 0;
            (void)usb_host_lib_handle_events(pdMS_TO_TICKS(usb_host_cfg::kEndPollMs), &flags);
        }
        Serial.printf("%s usb_host_uninstall: %s (0x%x) retries=%u\n", kTag, errorName(err), (unsigned)err,
                      (unsigned)retries);
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
        if (terr != ESP_OK) {
            // [USB4] Прибрати не вдалося: частину стеку лишено піднятою; begin() відмовлятиме до перезавантаження.
            s_needReboot.store(true);
            Serial.printf("%s install cleanup incomplete: reboot required\n", kTag);
        }
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
                  (int)usb_host_cfg::kTaskCore, libPrio(), mscPrio(), mgrPrio(), usb_host_cfg::kMountPath);
    USB_TRACE(StReady, true);  // [USB4]
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
                    // [USB4] Якщо драйвер MSC уже знято (s_mscUp = false), а host лишився піднятим — стек
                    // непридатний: begin() відмовляє (needsReboot()). Інакше (MSC ще працює, напр. пристрій
                    // відкритий) стек справний, end() можна повторити.
                    const bool degraded = !s_mscUp.load();
                    s_needReboot.store(degraded);
                    s_installed.store(s_mscUp.load() && s_hostUp.load());
                    if (degraded) {
                        Serial.printf("%s stack DEGRADED: MSC driver already uninstalled, USB host still up; "
                                      "recovery without reboot is NOT verified (end() may be retried)\n", kTag);
                    }
                    s_endOk.store(false);
                    s_life.store(Life::Up);
                    xSemaphoreGive(s_endDone);
                    break;
                }
                xQueueReset(s_queue);  // події, що прийшли під час зняття, вже не актуальні
                s_present.store(false);
                s_installed.store(false);
                s_needReboot.store(false);  // [USB4] успішне (в т.ч. повторне) зняття знімає позначку
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
        // [USB4] «Піднято» = host піднято І драйвер MSC працює І стек не позначено пошкодженим.
        result = s_installed.load() && s_hostUp.load() && s_mscUp.load() && !s_needReboot.load();
        if (!result) {
            Serial.printf("%s begin: stack not usable (installed=%d host_up=%d msc_up=%d need_reboot=%d); "
                          "reboot required (last error 0x%x at '%s')\n",
                          kTag, (int)s_installed.load(), (int)s_hostUp.load(), (int)s_mscUp.load(),
                          (int)s_needReboot.load(), (unsigned)lastError().code, lastError().where);
            if (lastError().code == 0) {
                setError(ESP_ERR_INVALID_STATE, "begin_degraded");
            }
            setState(State::Error);
        }
    } else if (life == Life::Off && s_needReboot.load()) {
        // [USB4] Прибирання після збійної інсталяції не вдалося: заново піднімати не можна.
        Serial.printf("%s begin: refused, previous cleanup failed (reboot required, last error 0x%x at '%s')\n",
                      kTag, (unsigned)lastError().code, lastError().where);
        setState(State::Error);
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
#if USB_HOST_DIAG
        traceReset();  // [USB4] t = 0 у момент чистого старту begin()
#endif
        s_life.store(Life::Starting);

        const BaseType_t ok = xTaskCreatePinnedToCore(mgrTask, "usb_mgr", usb_host_cfg::kMgrTaskStack, nullptr,
                                                      mgrPrio(), &s_mgrTask, usb_host_cfg::kTaskCore);
        if (ok != pdPASS) {
            Serial.printf("%s usb_mgr task create failed\n", kTag);
            s_mgrTask = nullptr;
            setError(ESP_ERR_NO_MEM, "mgr_task");
            setState(State::Error);
            s_life.store(Life::Off);
        } else if (xSemaphoreTake(s_installDone, pdMS_TO_TICKS(usb_host_cfg::kInstallWaitMs)) != pdTRUE) {
            Serial.printf("%s install wait timeout (%u ms); installation continues in background\n", kTag,
                          (unsigned)usb_host_cfg::kInstallWaitMs);
#if USB_HOST_DIAG
            printStageReport("begin: install wait timeout (stuck before 'ready')");
#endif
        } else {
            result = s_installed.load() && s_hostUp.load() && s_mscUp.load();
            if (!result) {
                Serial.printf("%s begin: install FAILED (last error 0x%x at '%s')\n", kTag,
                              (unsigned)lastError().code, lastError().where);
            }
        }
        USB_TRACE(StBeginReturned, true);  // [USB4]
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

bool needsReboot() { return s_needReboot.load(); }

bool isMounted() { return s_state.load() == State::Mounted; }

// [USB6] Блокування тому для fs::FS-обгортки (див. usb_msc_mount.h).
bool volumeLock(uint32_t waitMs) {
    if (s_ioMutex == nullptr) {
        return false;
    }
    return xSemaphoreTake(s_ioMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void volumeUnlock() {
    if (s_ioMutex != nullptr) {
        xSemaphoreGive(s_ioMutex);
    }
}

uint32_t volumeGenerationLocked() {
    return s_volReady.load() ? s_volGen.load() : 0;
}

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
    // [USB4] Стан «стек пошкоджено», чинні та дефолтні пріоритети, фонова задача MSC.
    Serial.printf("%s stat: need_reboot=%d prio lib/msc/mgr (effective, used by next begin)=%u/%u/%u "
                  "defaults=%u/%u/%u\n", kTag, (int)s_needReboot.load(), libPrio(), mscPrio(), mgrPrio(),
                  usb_host_cfg::kLibTaskPriority, usb_host_cfg::kMscTaskPriority, usb_host_cfg::kMgrTaskPriority);
    if (s_mscUp.load()) {
        printTaskInfo("USB MSC", xTaskGetHandle("USB MSC"), usb_host_cfg::kMscTaskStack);
    }
}

// [USB4] usb.prio <lib> <msc> <mgr>: пріоритети задач USB-host для НАСТУПНОГО begin() (без перезбірки).
// Без аргументів — чинні / дефолтні значення й пріоритети відомих задач (для порівняння); `usb.prio default` —
// повернути дефолти з usb_host_config.h. Дефолти в конфігу НЕ змінюються.
void printKnownTaskPrios() {
    static const char* const kNames[] = {"tcpip_thread", "wifi",       "esp_timer", "sys_evt", "async_tcp",
                                         "app_ctrl",     "loopTask",   "PeriodicTask", "line_path", "IDLE0",
                                         "IDLE1"};
    Serial.printf("%s prio: tasks found by name (prio only; core not shown):\n", kTag);
    for (const char* name : kNames) {
        const TaskHandle_t h = xTaskGetHandle(name);
        if (h != nullptr) {
            Serial.printf("%s prio:   %-14s %u\n", kTag, name, (unsigned)uxTaskPriorityGet(h));
        } else {
            Serial.printf("%s prio:   %-14s (not found)\n", kTag, name);
        }
    }
}

void cmdPrio(const char* arg) {
    const unsigned maxPrio = static_cast<unsigned>(configMAX_PRIORITIES - 1);
    if (arg == nullptr || arg[0] == '\0') {
        Serial.printf("%s prio: effective lib/msc/mgr=%u/%u/%u defaults=%u/%u/%u allowed 1..%u (used by NEXT begin)\n",
                      kTag, libPrio(), mscPrio(), mgrPrio(), usb_host_cfg::kLibTaskPriority,
                      usb_host_cfg::kMscTaskPriority, usb_host_cfg::kMgrTaskPriority, maxPrio);
        printKnownTaskPrios();
        return;
    }
    if (strncmp(arg, "default", 7) == 0) {
        s_prioLib.store(usb_host_cfg::kLibTaskPriority);
        s_prioMsc.store(usb_host_cfg::kMscTaskPriority);
        s_prioMgr.store(usb_host_cfg::kMgrTaskPriority);
        Serial.printf("%s prio: reset to defaults %u/%u/%u (used by NEXT begin)\n", kTag, libPrio(), mscPrio(),
                      mgrPrio());
        return;
    }
    unsigned a = 0;
    unsigned b = 0;
    unsigned d = 0;
    char extra = 0;
    if (sscanf(arg, "%u %u %u %c", &a, &b, &d, &extra) != 3) {
        Serial.printf("%s prio: usage usb.prio <lib> <msc> <mgr> | usb.prio default | usb.prio\n", kTag);
        return;
    }
    if (a < 1 || a > maxPrio || b < 1 || b > maxPrio || d < 1 || d > maxPrio) {
        Serial.printf("%s prio: each value must be 1..%u (configMAX_PRIORITIES - 1); nothing changed\n", kTag,
                      maxPrio);
        return;
    }
    s_prioLib.store(a);
    s_prioMsc.store(b);
    s_prioMgr.store(d);
    Serial.printf("%s prio: set lib/msc/mgr=%u/%u/%u for the NEXT begin()%s\n", kTag, a, b, d,
                  s_life.load() != Life::Off ? " (stack is up now: unchanged until usb.end + usb.begin)" : "");
}

// [USB4] usb.begin: після begin() (якщо не `nowait`) проба появи пристрою й монтування з мітками етапів.
// Проба опитує usb_host_lib_info() із loopTask паралельно з usb_lib (потокобезпечність за usb/usb_host.h НЕ
// перевірена: заголовок не прикладено; у teardown той самий виклик робить usb_mgr).
void probeAfterBegin(bool beginOk) {
    const uint32_t tp = millis();
    while ((millis() - tp) < usb_host_cfg::kLifePresentWaitMs) {
        if (s_stageMs[StConnected].load() != kNotReached) {
            break;
        }
        if (s_hostUp.load()) {
            usb_host_lib_info_t info = {};
            if (usb_host_lib_info(&info) == ESP_OK && info.num_devices > 0) {
                traceMark(StEnumSeen, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(usb_host_cfg::kProbePollMs));
    }
    const bool enumSeen = s_stageMs[StEnumSeen].load() != kNotReached;
    const bool connected = s_stageMs[StConnected].load() != kNotReached;
    if (connected) {
        const uint32_t tm = millis();
        while ((millis() - tm) < usb_host_cfg::kLifeMountWaitMs) {
            const State st = state();
            if (st == State::Mounted || st == State::Error || !s_present.load()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(usb_host_cfg::kProbePollMs));
        }
    }
    printStageReport("after usb.begin");
    const State st = state();
    if (!beginOk) {
        Serial.printf("%s VERDICT: begin() FAILED; furthest stage=%s; last error 0x%x (%s) at '%s'\n", kTag,
                      furthestStage(), (unsigned)lastError().code, errorName(lastError().code), lastError().where);
    } else if (connected && st == State::Mounted) {
        Serial.printf("%s VERDICT: ok, device mounted\n", kTag);
    } else if (connected && st == State::Error) {
        Serial.printf("%s VERDICT: device seen, mount FAILED (last error 0x%x at '%s')\n", kTag,
                      (unsigned)lastError().code, lastError().where);
    } else if (connected) {
        Serial.printf("%s VERDICT: STUCK after 'connected' (state=%s, furthest stage=%s): mount did not finish in %u ms\n",
                      kTag, stateName(st), furthestStage(), (unsigned)usb_host_cfg::kLifeMountWaitMs);
    } else if (enumSeen) {
        Serial.printf("%s VERDICT: STUCK: host library sees a device (num_devices>0) but the MSC 'connected' callback "
                      "did not arrive within %u ms (furthest stage=%s)\n",
                      kTag, (unsigned)usb_host_cfg::kLifePresentWaitMs, furthestStage());
    } else {
        Serial.printf("%s VERDICT: no device seen by host library within %u ms (no flash, or enumeration slower "
                      "than the wait)\n", kTag, (unsigned)usb_host_cfg::kLifePresentWaitMs);
    }
}

// [USB5] Результат одного циклу begin() -> пауза -> end() (для usb.beginend і usb.sweep).
struct BeginEndResult {
    bool okBegin;
    bool okEnd;
    uint32_t endMs;
    uint32_t freeBefore, largestBefore;
    uint32_t freeAfter, largestAfter;
    bool needReboot;
    State stateAfter;
};

// begin() і через <ms> мс від його виклику — end(). verbose: докладні рядки + звіт про етапи begin().
void beginEndOnce(uint32_t ms, bool verbose, BeginEndResult* r) {
    r->freeBefore = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    r->largestBefore = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t t0 = millis();
    r->okBegin = begin();
    const uint32_t spent = millis() - t0;
    if (spent < ms) {
        vTaskDelay(pdMS_TO_TICKS(ms - spent));
    }
    if (verbose) {
        Serial.printf("%s beginend: begin=%s, calling end() at t=%u ms (state=%s present=%d)\n", kTag,
                      r->okBegin ? "ok" : "FAILED", (unsigned)(millis() - t0), stateName(s_state.load()),
                      (int)s_present.load());
    }
    const uint32_t te = millis();
    r->okEnd = end();
    r->endMs = millis() - te;
    r->needReboot = s_needReboot.load();
    r->stateAfter = s_state.load();
    r->freeAfter = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    r->largestAfter = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (verbose) {
        Serial.printf("%s beginend: end=%s in %u ms (state=%s life=%s need_reboot=%d)\n", kTag,
                      r->okEnd ? "ok" : "FAILED", (unsigned)r->endMs, stateName(r->stateAfter),
                      lifeName(s_life.load()), (int)r->needReboot);
        Serial.printf("%s beginend: heap internal free/largest before %u/%u, after %u/%u\n", kTag,
                      (unsigned)r->freeBefore, (unsigned)r->largestBefore, (unsigned)r->freeAfter,
                      (unsigned)r->largestAfter);
        printStageReport("after usb.beginend (stages reached before end)");
    }
}

// [USB4] usb.beginend <ms>: begin() і через <ms> мс від його виклику — end(); ОДНА команда, тож час між ними
// детермінований (вставка двох рядків у монітор не годиться: рядки склеюються). Орієнтири з логів (флешка вставлена,
// radio): enum_seen ≈ 340–400, connected ≈ 410–510, dev_installed ≈ 690–790, mounted ≈ 700–800 мс.
void cmdBeginEnd(const char* arg) {
    const long ms = (arg != nullptr && arg[0] != '\0') ? strtol(arg, nullptr, 10) : -1;
    if (ms < 0 || ms > 3000) {
        Serial.printf("%s beginend: usage usb.beginend <ms 0..3000>\n", kTag);
        return;
    }
    if (s_life.load() != Life::Off) {
        Serial.printf("%s beginend: need life=Off (run usb.end first), now %s\n", kTag, lifeName(s_life.load()));
        return;
    }
    BeginEndResult r = {};
    beginEndOnce((uint32_t)ms, true, &r);
}

// [USB5] usb.sweep <від> <до> <крок> [прогонів 1..10]: послідовність begin() -> пауза -> end() для кожного
// значення паузи (мс). Результат КОЖНОГО кроку друкується до наступного (з Serial.flush()); перед кроком у RTC-памʼять
// (RTC_NOINIT, переживає перезавантаження після аборту) пишеться «крок у роботі»: після аборту / перезавантаження
// pollConsole() друкує, на якому кроці він стався. Якщо end() не вдався — sweep зупиняється сам.
// УВАГА: блокує викликача (loopTask) на весь sweep; лише для USB_HOST_DIAG.
constexpr uint32_t kSweepMagic = 0x55534235u;  // «USB5»
struct SweepRtc {
    uint32_t magic;
    uint32_t inStep;  // 1 — крок розпочато й не завершено
    int32_t from, to, step, runs;
    int32_t cur, run;
};
RTC_NOINIT_ATTR SweepRtc s_sweepRtc;

void cmdSweep(const char* arg) {
    long from = -1, to = -1, step = -1, runs = 1;
    const int n = (arg != nullptr) ? sscanf(arg, "%ld %ld %ld %ld", &from, &to, &step, &runs) : 0;
    if (n < 3 || from < 0 || from > 3000 || to < from || to > 3000 || step < 1 || runs < 1 || runs > 10) {
        Serial.printf("%s sweep: usage usb.sweep <from 0..3000> <to> <step >=1> [runs 1..10]\n", kTag);
        return;
    }
    if (s_life.load() != Life::Off) {
        Serial.printf("%s sweep: need life=Off (run usb.end first), now %s\n", kTag, lifeName(s_life.load()));
        return;
    }
    s_sweepRtc.magic = kSweepMagic;
    s_sweepRtc.from = (int32_t)from;
    s_sweepRtc.to = (int32_t)to;
    s_sweepRtc.step = (int32_t)step;
    s_sweepRtc.runs = (int32_t)runs;
    Serial.printf("%s SWEEP start from=%ld to=%ld step=%ld runs=%ld (device presence is NOT checked: note it)\n",
                  kTag, from, to, step, runs);
    uint32_t steps = 0;
    for (long ms = from; ms <= to; ms += step) {
        for (long run = 1; run <= runs; ++run) {
            s_sweepRtc.cur = (int32_t)ms;
            s_sweepRtc.run = (int32_t)run;
            s_sweepRtc.inStep = 1;
            Serial.printf("%s SWEEP step ms=%ld run=%ld/%ld BEGIN\n", kTag, ms, run, runs);
            Serial.flush();
            BeginEndResult r = {};
            beginEndOnce((uint32_t)ms, false, &r);
            s_sweepRtc.inStep = 0;
            ++steps;
            Serial.printf("%s SWEEP step ms=%ld run=%ld/%ld RESULT begin=%s end=%s end_ms=%u need_reboot=%d "
                          "state=%s free_before=%u largest_before=%u free_after=%u largest_after=%u d_free=%d\n",
                          kTag, ms, run, runs, r.okBegin ? "ok" : "FAILED", r.okEnd ? "ok" : "FAILED",
                          (unsigned)r.endMs, (int)r.needReboot, stateName(r.stateAfter), (unsigned)r.freeBefore,
                          (unsigned)r.largestBefore, (unsigned)r.freeAfter, (unsigned)r.largestAfter,
                          (int)r.freeAfter - (int)r.freeBefore);
            Serial.flush();
            if (!r.okBegin || !r.okEnd) {
                Serial.printf("%s SWEEP STOPPED at ms=%ld run=%ld: %s failed (last error 0x%x '%s'); no abort/reboot "
                              "happened\n", kTag, ms, run, !r.okBegin ? "begin()" : "end()",
                              (unsigned)lastError().code, lastError().where);
                return;
            }
        }
    }
    Serial.printf("%s SWEEP done: %u steps, all begin()/end() ok, no abort\n", kTag, (unsigned)steps);
}

// Друкується один раз після завантаження (з першого pollConsole): якщо попередній sweep обірвався під час кроку.
void reportSweepAfterBoot() {
    const esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_POWERON || s_sweepRtc.magic != kSweepMagic) {
        memset(&s_sweepRtc, 0, sizeof(s_sweepRtc));  // RTC_NOINIT після повного вимкнення — сміття
        s_sweepRtc.magic = kSweepMagic;
        return;
    }
    if (s_sweepRtc.inStep != 0) {
        Serial.printf("%s SWEEP ABORTED: reset (reason=%d, 4=panic/abort) during step ms=%d run=%d "
                      "(sweep from=%d to=%d step=%d runs=%d). Sweep was NOT finished.\n", kTag, (int)rr,
                      (int)s_sweepRtc.cur, (int)s_sweepRtc.run, (int)s_sweepRtc.from, (int)s_sweepRtc.to,
                      (int)s_sweepRtc.step, (int)s_sweepRtc.runs);
        s_sweepRtc.inStep = 0;
    }
}

void cmdBegin(const char* arg) {
    const bool nowait = (arg != nullptr) && strncmp(arg, "nowait", 6) == 0;
    const uint32_t t0 = millis();
    const bool ok = begin();
    Serial.printf("%s begin: %s in %u ms\n", kTag, ok ? "ok" : "failed (see usb.stat / usb.trace)",
                  (unsigned)(millis() - t0));
    if (!nowait) {
        probeAfterBegin(ok);
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
    Serial.printf("%s end: %s in %u ms (state=%s life=%s need_reboot=%d)\n", kTag,
                  ok ? "ok" : "failed (see usb.stat)", (unsigned)(millis() - t0), stateName(s_state.load()),
                  lifeName(s_life.load()), (int)s_needReboot.load());
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
    // [USB4] Відлуння: видно, що саме розібрано (рядки в терміналі можуть склеюватись).
    Serial.printf("%s got: %s\n", kTag, line);
    if (strstr(line + 1, "usb.") != nullptr) {
        Serial.printf("%s WARNING: several commands glued in one line - send one command per line\n", kTag);
    }
    if (isCmd(line, "usb.stat")) {
        cmdStat();
    } else if (isCmd(line, "usb.heap")) {
        printHeap("manual");
        usb_memdiag::print("usb.heap", true);  // [USB13] класи пам'яті + перевірка цілісності куп
        usb_memdiag::printFailStats();
        {
            const usb_fs::HandleStats h = usb_fs::handleStats();
            Serial.printf("%s fs handles: open=%lu closed_at_unmount=%lu deferred_total=%lu leaked_total=%lu\n", kTag,
                          (unsigned long)h.open, (unsigned long)h.closedAtUnmount, (unsigned long)h.deferredTotal,
                          (unsigned long)h.leakedTotal);
        }
    } else if (isCmd(line, "usb.ls")) {
        cmdLs(skipSpaces(line + strlen("usb.ls")));
    } else if (isCmd(line, "usb.cycle")) {
        cmdCycle(skipSpaces(line + strlen("usb.cycle")));
    } else if (isCmd(line, "usb.life")) {
        cmdLife(skipSpaces(line + strlen("usb.life")));
    } else if (isCmd(line, "usb.begin")) {
        cmdBegin(skipSpaces(line + strlen("usb.begin")));
    } else if (isCmd(line, "usb.beginend")) {
        cmdBeginEnd(skipSpaces(line + strlen("usb.beginend")));
    } else if (isCmd(line, "usb.sweep")) {
        cmdSweep(skipSpaces(line + strlen("usb.sweep")));
    } else if (isCmd(line, "usb.prio")) {
        cmdPrio(skipSpaces(line + strlen("usb.prio")));
    } else if (isCmd(line, "usb.trace")) {
        printStageReport("manual");
    } else if (isCmd(line, "usb.end")) {
        cmdEnd();
    } else if (isCmd(line, "usb.help")) {
        Serial.printf("%s commands: usb.stat | usb.ls [path] | usb.cycle N | usb.life N | usb.heap | "
                      "usb.begin [nowait] | usb.beginend <ms> | usb.sweep <from> <to> <step> [runs] | usb.end | usb.prio [<lib> <msc> <mgr> | default] | usb.trace | "
                      "usb.play <path> [start_sec] | usb.pstat | usb.seek <sec> | usb.stopf | usb.ended [take] | usb.fx [extra <ms> | dig <ms|-1> | level <dB>]\n",
                      kTag);
#if USB_HOST_DIAG
    } else if (usb_play_diag::isPlayCommand(line)) {
        usb_play_diag::handleConsoleLine(line);  // [USB7]
    } else if (strncmp(line, "usb.fs.", 7) == 0) {
        usb_fs::handleConsoleLine(line);  // [USB6]
#endif
    } else {
        Serial.printf("%s unknown command (usb.help)\n", kTag);
    }
}

}  // namespace

void pollConsole() {
    static char line[usb_host_cfg::kConsoleLineMax];
    static size_t len = 0;
    static bool s_bootReported = false;
    if (!s_bootReported) {
        s_bootReported = true;
        reportSweepAfterBoot();  // [USB5]
    }
    while (Serial.available() > 0) {
        // Не чіпаємо рядки інших консолей (xsmt.*, lp.*, st.*, set.* …): беремо байт лише якщо рядок
        // уже почато нами або починається з 'u'. (Інші консолі, що читають Serial з власних задач,
        // можуть забрати й наш рядок - див. «Відомі обмеження».)
        if (len == 0 && Serial.peek() != 'u') {
            // [USB14] Службові байти (CR / LF / пробіл / табуляція / керуючі), що стоять на початку рядка, забираємо й
            // відкидаємо: раніше зайвий CR / LF лишався в буфері Serial, а ми повертались, і консоль мовчала до скидання
            // плати. Друковані символи (початок рядка іншої консолі: xsmt.*, lp.*, st.*, set.*) НЕ чіпаємо, як і раніше.
            const int p = Serial.peek();
            if (p >= 0 && p < 0x21) {
                (void)Serial.read();
                continue;
            }
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
