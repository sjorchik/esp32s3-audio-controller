// [USB9] Режим «USB» — бекенд (крок g1). Див. usb_mode.h і config/usb_mode_config.h.
//
// Послідовності (виконує одноразова задача usb_mode, ядро 0, пріоритет 1):
//   ВХІД : AppController::usbModeEnter() [тиха зупинка радіо / звільнення проходу, прапорець режиму] -> чекаємо тишу
//          (AudioPlayer Idle, line_path без ресурсів, I2S0 не зафіксовано) -> пауза kRamSettleMs -> вимір RAM ->
//          usb_msc_mount::begin() -> чекаємо змонтування АБО «флешки нема» -> Ready.
//   ВИХІД: зупинка файлу (AudioPlayer::stop() -> Idle) -> usb_msc_mount::end() -> пауза kRamSettleMs -> вимір RAM ->
//          AppController::usbModeLeave() [відновлення попереднього входу штатним шляхом] -> Off.
// Помилки: збій begin() — відкат (вихід, як після невдалого входу), Off з причиною; збій end() — Error (стек може лишитись
// піднятим: usb_msc_mount::needsReboot()); повтор end() лише за lastError().where = «end_not_quiet…».

#include "usb/usb_mode.h"

#if FEATURE_USB_MODE

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "audio/audio_player.h"
#include "audio/line_path.h"
#include "config/usb_mode_config.h"
#include "core/app_controller.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_msc_mount.h"
#if USB_HOST_DIAG
#include "usb/usb_browser.h"  // [USB13] статус usb_nav при виході
#include "usb/usb_fs.h"       // [USB13] лічильники дескрипторів
#include "usb/usb_memdiag.h"  // [USB13] знімки пам'яті (DEF-5)
#endif

namespace usb_mode {

namespace {

constexpr const char* kTag = "[USBM]";

// Операції фонової задачі.
enum class Op : uint8_t { Enter, Exit };

// --- Стан (потокобезпечно) -----------------------------------------------------------------------------------------
// Внутрішній стан зберігається БЕЗ Playing (похідний). Запис — лише під s_mux / атомарно; читання state() — атомарно.
std::atomic<uint8_t> s_state{static_cast<uint8_t>(State::Off)};
std::atomic<bool> s_busy{false};        // фонова задача існує (одна операція за раз)
std::atomic<bool> s_exitQueued{false};  // exit запитано під час Entering
std::atomic<uint8_t> s_exitCause{static_cast<uint8_t>(ExitCause::User)};
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Info s_info = {};  // під s_mux (поле state тут НЕ використовується — береться з s_state)

constexpr int16_t kNoPrev = -1;

#if USB_HOST_DIAG
// [USB13] Знімок пам'яті ДО входу (для різниці при виході; у т.ч. PSRAM).
usb_memdiag::Snap s_memEnter = {};
bool s_memEnterValid = false;
#endif

inline State rawState() { return static_cast<State>(s_state.load()); }

void setState(State s) {
    const State prev = static_cast<State>(s_state.exchange(static_cast<uint8_t>(s)));
    if (prev != s) {
        Serial.printf("%s state %s -> %s\n", kTag, stateName(prev), stateName(s));
    }
}

RamSample ramNow() {
    RamSample r;
    r.freeBytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    r.largestBlock = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    r.minFreeBytes = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    return r;
}

void printRam(const char* label, const RamSample& r) {
    Serial.printf("%s ram[%s] internal free=%u largest=%u min_free=%u\n", kTag, label, static_cast<unsigned>(r.freeBytes),
                  static_cast<unsigned>(r.largestBlock), static_cast<unsigned>(r.minFreeBytes));
}

inline uint32_t since(uint32_t t0) { return static_cast<uint32_t>(millis() - t0); }

Media mediaNow() {
    switch (usb_msc_mount::state()) {
        case usb_msc_mount::State::Mounted:  return Media::Mounted;
        case usb_msc_mount::State::Mounting: return Media::Mounting;
        case usb_msc_mount::State::Error:    return Media::Fault;
        case usb_msc_mount::State::NoDevice:
        case usb_msc_mount::State::Off:      break;
    }
    return Media::None;
}

// Запис у s_info під критичною секцією. fn не має блокувати.
template <typename F>
void infoWrite(F fn) {
    portENTER_CRITICAL(&s_mux);
    fn(s_info);
    portEXIT_CRITICAL(&s_mux);
}

void infoResetForEnter() {
    infoWrite([](Info& i) {
        const RamSample keepNothing = {};
        i = Info{};
        i.prevInput = kNoPrev;
        i.ramBeforeEnter = keepNothing;
    });
}

void setReason(const char* reason, const char* detail) {
    infoWrite([&](Info& i) {
        i.reason = reason;
        i.detail = detail;
    });
}

// Помилка кроку з логом. Статичні рядки.
void logFail(const char* what, const char* detail) {
    Serial.printf("%s FAIL: %s%s%s\n", kTag, what, detail != nullptr ? " - " : "", detail != nullptr ? detail : "");
}

// Повтори виклику AppController (мʼютекс контролера може бути зайнятий довше kLockTimeoutMs).
bool appEnter(AppController::UsbModeSnapshot* prev, const char** why) {
    for (uint8_t i = 0; i < usb_mode_cfg::kAppCallRetries; ++i) {
        const char* w = nullptr;
        if (AppController::usbModeEnter(prev, &w)) {
            return true;
        }
        if (why != nullptr) {
            *why = w;
        }
        // Повторюємо лише «busy» (мʼютекс); решта причин — рішення контролера.
        if (w == nullptr || strcmp(w, "busy") != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kAppCallRetryMs));
    }
    return false;
}

bool appLeave(AppController::UsbModeSnapshot* restored) {
    for (uint8_t i = 0; i < usb_mode_cfg::kAppCallRetries; ++i) {
        if (AppController::usbModeLeave(restored)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kAppCallRetryMs));
    }
    return false;
}

// Тиша: радіо в Idle, прохід без ресурсів і не в Running, I2S0 не зафіксовано на 48 кГц (інакше playFile відмовить).
bool isQuiet() {
    return AudioPlayer::state() == PlayerState::Idle && !LinePath::resourcesUp() && !LinePath::isRunning() &&
           !AudioPlayer::fixedOutputRate();
}

bool waitQuiet(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (!isQuiet()) {
        if (since(t0) >= timeoutMs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kPollMs));
    }
    return true;
}

// Повтор end() дозволено лише коли стек справний (USB5): end_not_quiet / end_not_quiet_post.
bool endRetryable() {
    const char* w = usb_msc_mount::lastError().where;
    return w != nullptr && (strcmp(w, "end_not_quiet") == 0 || strcmp(w, "end_not_quiet_post") == 0);
}

// Вихід (також після невдалого входу). Викликається з задачі usb_mode; стан на вході: Entering або Exiting.
void runExit(ExitCause cause) {
    const uint32_t tExit = millis();
    setState(State::Exiting);
    Serial.printf("%s exit: begin (cause=%s)\n", kTag, cause == ExitCause::Standby ? "standby" : "user");

    // 1. Зупинка файлу (штатно: мʼют ЦАП із паузою, stopSong; радіо-DSP повертає сам плеєр).
    const uint32_t tStop = millis();
    bool accepted = false;
    bool idle = false;
    while (since(tStop) < usb_mode_cfg::kFileStopWaitMs) {
        if (AudioPlayer::state() == PlayerState::Idle && !AudioPlayer::fileActive()) {
            idle = true;
            break;
        }
        if (!accepted) {
            accepted = AudioPlayer::stop();
        }
        vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kPollMs));
    }
    const uint32_t fileStopMs = since(tStop);
    if (!idle) {
        Serial.printf("%s WARNING: player not Idle after %u ms (state=%s) - continuing with end()\n", kTag,
                      static_cast<unsigned>(fileStopMs), AudioPlayer::stateName(AudioPlayer::state()));
    }

#if USB_HOST_DIAG
    {  // [USB13] стан ПІСЛЯ зупинки файла, ДО end(): дескриптори й usb_nav
        usb_memdiag::print("exit:file stopped", true);
        const usb_fs::HandleStats h = usb_fs::handleStats();
        usb_browser::Status nb = {};
        const bool nbOk = usb_browser::status(&nb);
        Serial.printf("%s exit: fs handles open=%u closed_at_unmount=%u deferred_total=%u leaked_total=%u | usb_nav active=%d%s\n",
                      kTag, (unsigned)h.open, (unsigned)h.closedAtUnmount, (unsigned)h.deferredTotal,
                      (unsigned)h.leakedTotal, nbOk ? (int)nb.active : -1, nbOk ? "" : " (status busy)");
    }
#endif

    // 2. Зняття USB-host (0,6 – 1,25 с, до kEndWaitMs) — у цій фоновій задачі, не з UI.
    const RamSample ramPreEnd = ramNow();
    printRam("exit: before end()", ramPreEnd);
    const uint32_t tEnd = millis();
    const bool endOk = usb_msc_mount::end();
    const uint32_t endMs = since(tEnd);
    infoWrite([&](Info& i) {
        i.fileStopMs = fileStopMs;
        i.endMs = endMs;
    });

    if (!endOk) {
        const usb_msc_mount::LastError le = usb_msc_mount::lastError();
        const bool reboot = usb_msc_mount::needsReboot();
        Serial.printf("%s ERROR: end() FAILED after %u ms (last error 0x%x at '%s'), need_reboot=%d\n", kTag,
                      static_cast<unsigned>(endMs), static_cast<unsigned>(le.code), le.where != nullptr ? le.where : "?",
                      reboot ? 1 : 0);
        if (reboot) {
            Serial.printf("%s ERROR: USB stack is degraded and stays up - reboot the device\n", kTag);
        } else if (endRetryable()) {
            Serial.printf("%s ERROR: stack is intact - repeat 'usb.mode exit' to retry end()\n", kTag);
        }
        infoWrite([&](Info& i) {
            i.reason = "end_failed";
            i.detail = le.where;
            i.exitMs = since(tExit);
            i.taskStackMinFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        });
        // Standby: пристрій і так «вимкнений» — знімаємо прапорець режиму, щоб POWER міг вивести його зі standby
        // (стек лишається піднятим, стан usb_mode = Error, тож видно, що RAM не повернулась).
        if (cause == ExitCause::Standby) {
            AppController::UsbModeSnapshot ignore = {};
            if (!appLeave(&ignore)) {
                Serial.printf("%s WARNING: AppController::usbModeLeave failed after a failed end()\n", kTag);
            }
        }
        s_busy.store(false);
        setState(State::Error);
        return;
    }

    // 3. RAM після end(): порівняння з виміром ПІСЛЯ «тиші» перед begin() (той самий стан: нема ні радіо, ні host).
    vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kRamSettleMs));
    const RamSample ramAfter = ramNow();
    Info snap;
    portENTER_CRITICAL(&s_mux);
    snap = s_info;
    portEXIT_CRITICAL(&s_mux);
    printRam("exit: after end()", ramAfter);
#if USB_HOST_DIAG
    {  // [USB13] знімок ПІСЛЯ end(), ДО старту радіо: порівняння з «enter:before» за ВСІМА класами (PSRAM теж)
        const usb_memdiag::Snap after = usb_memdiag::print("exit:after end() (before radio restore)", true);
        if (s_memEnterValid) {
            usb_memdiag::printDelta("exit vs enter:before", s_memEnter, after);
        }
        usb_browser::Status nb = {};
        const bool nbOk = usb_browser::status(&nb);
        Serial.printf("%s exit: usb_nav active=%d%s\n", kTag, nbOk ? (int)nb.active : -1, nbOk ? "" : " (status busy)");
    }
#endif
    if (snap.ramQuiet.freeBytes != 0) {
        Serial.printf("%s ram vs before begin(): d_free=%ld d_largest=%ld d_min=%ld (expect 0; first begin/end after boot: -128 B)\n",
                      kTag, static_cast<long>(ramAfter.freeBytes) - static_cast<long>(snap.ramQuiet.freeBytes),
                      static_cast<long>(ramAfter.largestBlock) - static_cast<long>(snap.ramQuiet.largestBlock),
                      static_cast<long>(ramAfter.minFreeBytes) - static_cast<long>(snap.ramQuiet.minFreeBytes));
    }

    // 4. Відновлення попереднього входу штатним шляхом (RAM для TLS / line_path уже вільна).
    const uint32_t tRestore = millis();
    AppController::UsbModeSnapshot restored = {};
    const bool leftOk = appLeave(&restored);
    const uint32_t restoreMs = since(tRestore);
    if (!leftOk) {
        logFail("AppController::usbModeLeave refused", "controller busy");
        infoWrite([&](Info& i) {
            i.ramAfterEnd = ramAfter;
            i.restoreMs = restoreMs;
            i.exitMs = since(tExit);
            i.reason = "restore_failed";
            i.detail = "controller_busy";
            i.taskStackMinFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        });
        s_busy.store(false);
        setState(State::Error);
        return;
    }
    Serial.printf("%s exit: done in %u ms (file_stop=%u end=%u restore=%u) -> input=%u station=%u%s\n", kTag,
                  static_cast<unsigned>(since(tExit)), static_cast<unsigned>(fileStopMs), static_cast<unsigned>(endMs),
                  static_cast<unsigned>(restoreMs), static_cast<unsigned>(restored.input),
                  static_cast<unsigned>(restored.station), cause == ExitCause::Standby ? " (standby: nothing restored)" : "");
    infoWrite([&](Info& i) {
        i.ramAfterEnd = ramAfter;
        i.restoreMs = restoreMs;
        i.exitMs = since(tExit);
        i.reason = nullptr;
        i.detail = nullptr;
        i.exitQueued = false;
        i.taskStackMinFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    });
    s_busy.store(false);
    setState(State::Off);
}

// Відкат невдалого входу: стек знімається (якщо піднято), попередній стан відновлюється, стан Off з причиною.
void rollbackEnter(const char* reason, const char* detail, uint32_t tEnter) {
    Serial.printf("%s enter: rolling back (%s)\n", kTag, reason);
    bool stackOk = true;
    if (usb_msc_mount::isStarted() || usb_msc_mount::state() != usb_msc_mount::State::Off) {
        stackOk = usb_msc_mount::end();
    }
    if (!stackOk) {
        const usb_msc_mount::LastError le = usb_msc_mount::lastError();
        Serial.printf("%s ERROR: end() failed during rollback (0x%x at '%s') - stay in USB mode (silent)\n", kTag,
                      static_cast<unsigned>(le.code), le.where != nullptr ? le.where : "?");
        infoWrite([&](Info& i) {
            i.reason = reason;
            i.detail = le.where;
            i.enterMs = since(tEnter);
            i.taskStackMinFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        });
        s_busy.store(false);
        setState(State::Error);
        return;
    }
    AppController::UsbModeSnapshot restored = {};
    const bool leftOk = appLeave(&restored);
    infoWrite([&](Info& i) {
        i.reason = reason;
        i.detail = detail;
        i.enterMs = since(tEnter);
        i.taskStackMinFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        if (!leftOk) {
            i.reason = "restore_failed";
            i.detail = "controller_busy";
        }
    });
    s_busy.store(false);
    setState(leftOk ? State::Off : State::Error);
}

void runEnter() {
    const uint32_t tEnter = millis();
    infoResetForEnter();
    const RamSample ram0 = ramNow();
    infoWrite([&](Info& i) { i.ramBeforeEnter = ram0; });
    printRam("enter: before", ram0);
#if USB_HOST_DIAG
    usb_memdiag::installFailHook();  // [USB13] хук невдалих виділень діє на весь сеанс (і на старт радіо після виходу)
    s_memEnter = usb_memdiag::print("enter:before", true);
    s_memEnterValid = true;
#endif

    // 1. Тиха зупинка радіо / звільнення проходу штатними шляхами AppController (запамʼятовує попередній стан).
    AppController::UsbModeSnapshot prev = {};
    const char* why = nullptr;
    if (!appEnter(&prev, &why)) {
        logFail("enter refused by AppController", why);
        infoWrite([&](Info& i) {
            i.reason = "enter_refused";
            i.detail = why;
            i.enterMs = since(tEnter);
        });
        s_busy.store(false);
        setState(State::Off);
        return;
    }
    infoWrite([&](Info& i) {
        i.prevInput = static_cast<int16_t>(prev.input);
        i.prevStation = prev.station;
        i.prevRadioPaused = prev.radioPaused;
    });
    Serial.printf("%s enter: prev input=%u station=%u radio_paused=%d\n", kTag, static_cast<unsigned>(prev.input),
                  static_cast<unsigned>(prev.station), prev.radioPaused ? 1 : 0);

    // 2. Чекаємо тишу (радіо Idle, прохід звільнено, I2S0 розфіксовано), потім пауза й вимір RAM.
    const uint32_t tQuiet = millis();
    if (!waitQuiet(usb_mode_cfg::kQuietWaitMs)) {
        Serial.printf("%s enter: not quiet after %u ms (player=%s line_resources=%d line_running=%d fixed48=%d)\n", kTag,
                      static_cast<unsigned>(since(tQuiet)), AudioPlayer::stateName(AudioPlayer::state()),
                      LinePath::resourcesUp() ? 1 : 0, LinePath::isRunning() ? 1 : 0,
                      AudioPlayer::fixedOutputRate() ? 1 : 0);
        rollbackEnter("quiet_timeout", nullptr, tEnter);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kRamSettleMs));
    const uint32_t quietMs = since(tQuiet);
    const RamSample ramQuiet = ramNow();
    infoWrite([&](Info& i) {
        i.quietMs = quietMs;
        i.ramQuiet = ramQuiet;
    });
    printRam("enter: quiet (before begin)", ramQuiet);
#if USB_HOST_DIAG
    usb_memdiag::print("enter:quiet");  // [USB13]
#endif
    // Поріг не вигадується: друкуємо виміряну різницю; «не зросла» при працювавшому радіо / проході — підозра.
    Serial.printf("%s ram freed by stopping the source: d_free=%ld d_largest=%ld (quiet in %u ms)\n", kTag,
                  static_cast<long>(ramQuiet.freeBytes) - static_cast<long>(ram0.freeBytes),
                  static_cast<long>(ramQuiet.largestBlock) - static_cast<long>(ram0.largestBlock),
                  static_cast<unsigned>(quietMs));

    // 3. USB-host.
    const uint32_t tBegin = millis();
    const bool beginOk = usb_msc_mount::begin();
    const uint32_t beginMs = since(tBegin);
    infoWrite([&](Info& i) { i.beginMs = beginMs; });
    if (!beginOk) {
        const usb_msc_mount::LastError le = usb_msc_mount::lastError();
        Serial.printf("%s enter: begin() FAILED in %u ms (last error 0x%x at '%s', need_reboot=%d)\n", kTag,
                      static_cast<unsigned>(beginMs), static_cast<unsigned>(le.code),
                      le.where != nullptr ? le.where : "?", usb_msc_mount::needsReboot() ? 1 : 0);
        rollbackEnter("begin_failed", le.where, tEnter);
        return;
    }

    // 4. Очікуємо носій: фаза 1 — поява пристрою (kEnterPresenceWaitMs), фаза 2 — монтування (kEnterMountWaitMs).
    const uint32_t tMount = millis();
    uint32_t limit = tMount + usb_mode_cfg::kEnterPresenceWaitMs;
    bool mountingSeen = false;
    for (;;) {
        const usb_msc_mount::State us = usb_msc_mount::state();
        if (us == usb_msc_mount::State::Mounted || us == usb_msc_mount::State::Error) {
            break;  // змонтовано або монтування не вдалося (Media::Fault) — лишаємось у режимі
        }
        if (us == usb_msc_mount::State::Mounting && !mountingSeen) {
            mountingSeen = true;
            limit = millis() + usb_mode_cfg::kEnterMountWaitMs;
        }
        if (us == usb_msc_mount::State::NoDevice && mountingSeen) {
            break;  // вийняли під час монтування
        }
        if (static_cast<int32_t>(millis() - limit) >= 0) {
            break;  // флешки нема (або монтування не вклалось у вікно — стан лишається Media::Mounting)
        }
        if (s_exitQueued.load()) {
            break;  // exit запитано під час входу
        }
        vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kPollMs));
    }
    const uint32_t mountMs = since(tMount);
    const RamSample ramReady = ramNow();
    const Media media = mediaNow();
    infoWrite([&](Info& i) {
        i.mountMs = mountMs;
        i.enterMs = since(tEnter);
        i.ramReady = ramReady;
        i.taskStackMinFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        i.reason = nullptr;
        i.detail = nullptr;
    });
    printRam("enter: ready", ramReady);
#if USB_HOST_DIAG
    usb_memdiag::print("enter:ready");  // [USB13]
#endif
    Serial.printf("%s enter: ready in %u ms (quiet=%u begin=%u mount_wait=%u) media=%s; host cost d_free=%ld d_largest=%ld\n",
                  kTag, static_cast<unsigned>(since(tEnter)), static_cast<unsigned>(quietMs),
                  static_cast<unsigned>(beginMs), static_cast<unsigned>(mountMs), mediaName(media),
                  static_cast<long>(ramReady.freeBytes) - static_cast<long>(ramQuiet.freeBytes),
                  static_cast<long>(ramReady.largestBlock) - static_cast<long>(ramQuiet.largestBlock));
    if (media == Media::Fault) {
        const usb_msc_mount::LastError le = usb_msc_mount::lastError();
        Serial.printf("%s enter: media fault (last error 0x%x at '%s') - staying in USB mode, re-insert the drive\n", kTag,
                      static_cast<unsigned>(le.code), le.where != nullptr ? le.where : "?");
    }

    if (s_exitQueued.exchange(false)) {
        Serial.printf("%s enter: exit was queued during entering - exiting now\n", kTag);
        runExit(static_cast<ExitCause>(s_exitCause.load()));
        return;
    }
    s_busy.store(false);
    setState(State::Ready);
}

void opTask(void* arg) {
    const Op op = static_cast<Op>(reinterpret_cast<uintptr_t>(arg));
    if (op == Op::Enter) {
        runEnter();
    } else {
        runExit(static_cast<ExitCause>(s_exitCause.load()));
    }
    vTaskDelete(nullptr);
}

// Створює фонову задачу. Викликач вже поставив s_busy = true й потрібний стан.
bool spawn(Op op) {
    const BaseType_t ok = xTaskCreatePinnedToCore(opTask, usb_mode_cfg::kTaskName, usb_mode_cfg::kTaskStack,
                                                  reinterpret_cast<void*>(static_cast<uintptr_t>(op)),
                                                  usb_mode_cfg::kTaskPriority, nullptr, usb_mode_cfg::kTaskCore);
    return ok == pdPASS;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Публічний API
// ---------------------------------------------------------------------------------------------------------------------
const char* stateName(State s) {
    switch (s) {
        case State::Off:      return "Off";
        case State::Entering: return "Entering";
        case State::Ready:    return "Ready";
        case State::Playing:  return "Playing";
        case State::Exiting:  return "Exiting";
        case State::Error:    return "Error";
    }
    return "?";
}

const char* mediaName(Media m) {
    switch (m) {
        case Media::None:     return "none";
        case Media::Mounting: return "mounting";
        case Media::Mounted:  return "mounted";
        case Media::Fault:    return "fault";
    }
    return "?";
}

State state() {
    const State s = rawState();
    if (s == State::Ready && AudioPlayer::fileActive()) {
        return State::Playing;
    }
    return s;
}

bool isActive() { return rawState() != State::Off; }

Info info() {
    Info out;
    portENTER_CRITICAL(&s_mux);
    out = s_info;
    portEXIT_CRITICAL(&s_mux);
    out.state = state();
    out.media = (out.state == State::Off) ? Media::None : mediaNow();
    out.needsReboot = usb_msc_mount::needsReboot();
    out.exitQueued = s_exitQueued.load();
    return out;
}

Media media() { return rawState() == State::Off ? Media::None : mediaNow(); }  // [USB11]

bool requestEnter(const char** whyOut) {
    if (whyOut != nullptr) {
        *whyOut = nullptr;
    }
    const State st = rawState();
    if (st != State::Off) {
        Serial.printf("%s enter ignored: state=%s\n", kTag, stateName(st));
        if (whyOut != nullptr) {
            *whyOut = "not_off";
        }
        return false;
    }
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        Serial.printf("%s enter ignored: previous operation is still finishing\n", kTag);
        if (whyOut != nullptr) {
            *whyOut = "busy";
        }
        return false;
    }
    s_exitQueued.store(false);
    infoResetForEnter();
    setState(State::Entering);
    if (!spawn(Op::Enter)) {
        s_busy.store(false);
        setState(State::Off);
        setReason("task_create_failed", nullptr);
        Serial.printf("%s enter: task create failed (low internal RAM?)\n", kTag);
        if (whyOut != nullptr) {
            *whyOut = "task_create_failed";
        }
        return false;
    }
    return true;
}

bool requestExit(ExitCause cause) {
    const State st = rawState();
    switch (st) {
        case State::Off:
            Serial.printf("%s exit ignored: mode is off\n", kTag);
            return true;
        case State::Exiting:
            Serial.printf("%s exit ignored: already exiting\n", kTag);
            return true;
        case State::Entering:
            s_exitCause.store(static_cast<uint8_t>(cause));
            s_exitQueued.store(true);
            Serial.printf("%s exit queued: entering is in progress\n", kTag);
            return true;
        case State::Error:
            if (!endRetryable()) {
                Serial.printf("%s exit refused: USB stack needs a reboot (reason=%s, last error '%s')\n", kTag,
                              s_info.reason != nullptr ? s_info.reason : "?",
                              usb_msc_mount::lastError().where != nullptr ? usb_msc_mount::lastError().where : "?");
                if (cause == ExitCause::Standby) {
                    // Пристрій іде в standby: знімаємо лише прапорець режиму, щоб POWER міг його вивести.
                    AppController::UsbModeSnapshot ignore = {};
                    appLeave(&ignore);
                }
                return false;
            }
            break;  // повтор end(): як звичайний вихід
        case State::Ready:
        case State::Playing:
            break;
    }
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        Serial.printf("%s exit ignored: previous operation is still finishing\n", kTag);
        return true;
    }
    s_exitCause.store(static_cast<uint8_t>(cause));
    setState(State::Exiting);
    if (!spawn(Op::Exit)) {
        s_busy.store(false);
        setState(State::Error);
        setReason("task_create_failed", nullptr);
        Serial.printf("%s exit: task create failed\n", kTag);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Діагностична консоль
// ---------------------------------------------------------------------------------------------------------------------
#if USB_HOST_DIAG
namespace {

const char* skipWs(const char* s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

bool word(const char* s, const char* w, const char** rest) {
    const size_t n = strlen(w);
    if (strncmp(s, w, n) != 0 || (s[n] != '\0' && s[n] != ' ' && s[n] != '\t')) {
        return false;
    }
    *rest = skipWs(s + n);
    return true;
}

void cmdStat() {
    const Info i = info();
    Serial.printf("%s stat: state=%s media=%s reason=%s detail=%s prev_input=%d prev_station=%u radio_paused=%d "
                  "exit_queued=%d need_reboot=%d | usb=%s started=%d\n",
                  kTag, stateName(i.state), mediaName(i.media), i.reason != nullptr ? i.reason : "-",
                  i.detail != nullptr ? i.detail : "-", static_cast<int>(i.prevInput), static_cast<unsigned>(i.prevStation),
                  i.prevRadioPaused ? 1 : 0, i.exitQueued ? 1 : 0, i.needsReboot ? 1 : 0,
                  usb_msc_mount::stateName(usb_msc_mount::state()), usb_msc_mount::isStarted() ? 1 : 0);
    Serial.printf("%s stat: player=%s source=%s file_active=%d file_err=%s | line_resources=%d line=%s fixed48=%d | app_usb_mode=%d\n",
                  kTag, AudioPlayer::stateName(AudioPlayer::state()), AudioPlayer::isFileSource() ? "file" : "radio",
                  AudioPlayer::fileActive() ? 1 : 0,
                  AudioPlayer::lastFileError() != nullptr ? AudioPlayer::lastFileError() : "-",
                  LinePath::resourcesUp() ? 1 : 0, LinePath::stateName(LinePath::state()),
                  AudioPlayer::fixedOutputRate() ? 1 : 0, AppController::usbModeActive() ? 1 : 0);
    Serial.printf("%s stat: enter=%u ms (quiet=%u begin=%u mount_wait=%u) | exit=%u ms (file_stop=%u end=%u restore=%u) | "
                  "task stack min free=%u B\n",
                  kTag, static_cast<unsigned>(i.enterMs), static_cast<unsigned>(i.quietMs), static_cast<unsigned>(i.beginMs),
                  static_cast<unsigned>(i.mountMs), static_cast<unsigned>(i.exitMs), static_cast<unsigned>(i.fileStopMs),
                  static_cast<unsigned>(i.endMs), static_cast<unsigned>(i.restoreMs),
                  static_cast<unsigned>(i.taskStackMinFree));
    // [USB11] Навігація (коло входів / USB_MODE): відкладена ціль і лічильники.
    AppController::UsbNavStats nav = {};
    const bool navOk = AppController::usbNavStats(&nav);
    Serial.printf("%s stat: nav%s pending_target=%d (-1 none, 0..2 input, 3 usb, 4 previous) requests=%u replaced=%u "
                  "exit_failures=%u restarts=%u\n",
                  kTag, navOk ? "" : " (controller busy)", static_cast<int>(nav.pendingTarget),
                  static_cast<unsigned>(nav.requests), static_cast<unsigned>(nav.replaced),
                  static_cast<unsigned>(nav.exitFailures), static_cast<unsigned>(nav.restarts));
    printRam("now", ramNow());
    printRam("before enter", i.ramBeforeEnter);
    printRam("quiet (before begin)", i.ramQuiet);
    printRam("ready", i.ramReady);
    printRam("after end", i.ramAfterEnd);
}

// Відновлений вхід реально звучить? (радіо Playing / прохід Running); радіо на паузі — не чекаємо.
bool restoredRunning(const AppController::UsbModeSnapshot& prev) {
    if (prev.input == 0) {
        return prev.radioPaused || AudioPlayer::state() == PlayerState::Playing;
    }
    return LinePath::isStable();
}

void cmdCycle(const char* arg) {
    const long n = atol(arg);
    if (n < 1 || n > static_cast<long>(usb_mode_cfg::kCycleMaxCount)) {
        Serial.printf("%s usage: usb.mode cycle <N 1..%u>\n", kTag, static_cast<unsigned>(usb_mode_cfg::kCycleMaxCount));
        return;
    }
    if (rawState() != State::Off) {
        Serial.printf("%s cycle refused: state=%s (needs Off)\n", kTag, stateName(state()));
        return;
    }
    const RamSample base = ramNow();
    printRam("cycle baseline (before the first cycle)", base);
    Serial.printf("%s cycle: N=%ld (loopTask is blocked; the console is not read until it ends)\n", kTag, n);
    long done = 0;
    long maxAbsFree = 0;
    long maxAbsLargest = 0;
    long withDiff = 0;
    long inputMismatch = 0;
    for (long c = 1; c <= n; ++c) {
        const uint32_t tCycle = millis();
        const char* why = nullptr;
        if (!requestEnter(&why)) {
            Serial.printf("%s cycle %ld/%ld: FAIL enter refused (%s) - stopping\n", kTag, c, n, why != nullptr ? why : "?");
            break;
        }
        const uint32_t w0 = millis();
        while (rawState() == State::Entering && since(w0) < usb_mode_cfg::kCycleEnterWaitMs) {
            vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kPollMs));
        }
        Info inf = info();
        if (inf.state != State::Ready && inf.state != State::Playing) {
            Serial.printf("%s cycle %ld/%ld: FAIL after enter: state=%s reason=%s detail=%s - stopping\n", kTag, c, n,
                          stateName(inf.state), inf.reason != nullptr ? inf.reason : "-", inf.detail != nullptr ? inf.detail : "-");
            break;
        }
        const Media media = inf.media;
        AppController::UsbModeSnapshot prev = {};
        prev.input = static_cast<uint8_t>(inf.prevInput < 0 ? 0 : inf.prevInput);
        prev.station = inf.prevStation;
        prev.radioPaused = inf.prevRadioPaused;
        const uint32_t enterMs = inf.enterMs;
        requestExit(ExitCause::User);
        const uint32_t w1 = millis();
        while (rawState() == State::Exiting && since(w1) < usb_mode_cfg::kCycleExitWaitMs) {
            vTaskDelay(pdMS_TO_TICKS(usb_mode_cfg::kPollMs));
        }
        inf = info();
        if (inf.state != State::Off) {
            Serial.printf("%s cycle %ld/%ld: FAIL after exit: state=%s reason=%s detail=%s need_reboot=%d - stopping\n", kTag, c,
                          n, stateName(inf.state), inf.reason != nullptr ? inf.reason : "-",
                          inf.detail != nullptr ? inf.detail : "-", inf.needsReboot ? 1 : 0);
            break;
        }
        // Чекаємо, поки відновлений вхід зазвучить (щоб RAM порівнювалась у тому самому стані, що й базовий вимір).
        const uint32_t w2 = millis();
        while (!restoredRunning(prev) && since(w2) < usb_mode_cfg::kRestoreSettleMs) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        const bool running = restoredRunning(prev);
        const RamSample r = ramNow();
        const long dF = static_cast<long>(r.freeBytes) - static_cast<long>(base.freeBytes);
        const long dL = static_cast<long>(r.largestBlock) - static_cast<long>(base.largestBlock);
        const long dM = static_cast<long>(r.minFreeBytes) - static_cast<long>(base.minFreeBytes);
        ++done;
        if (labs(dF) > maxAbsFree) maxAbsFree = labs(dF);
        if (labs(dL) > maxAbsLargest) maxAbsLargest = labs(dL);
        if (dF != 0 || dL != 0) ++withDiff;
        const bool sameInput = (restoredRunning(prev) || prev.radioPaused);
        if (!sameInput) ++inputMismatch;
        Serial.printf("%s cycle %ld/%ld: media=%s enter=%u ms (quiet=%u begin=%u mount=%u) exit=%u ms (file_stop=%u end=%u restore=%u) "
                      "restored=%s (input=%u station=%u) cycle=%u ms | free=%u largest=%u min_free=%u | d_free=%ld d_largest=%ld d_min=%ld\n",
                      kTag, c, n, mediaName(media), static_cast<unsigned>(enterMs), static_cast<unsigned>(inf.quietMs),
                      static_cast<unsigned>(inf.beginMs), static_cast<unsigned>(inf.mountMs), static_cast<unsigned>(inf.exitMs),
                      static_cast<unsigned>(inf.fileStopMs), static_cast<unsigned>(inf.endMs),
                      static_cast<unsigned>(inf.restoreMs), running ? "running" : "NOT running (timeout)",
                      static_cast<unsigned>(prev.input), static_cast<unsigned>(prev.station), static_cast<unsigned>(since(tCycle)),
                      static_cast<unsigned>(r.freeBytes), static_cast<unsigned>(r.largestBlock),
                      static_cast<unsigned>(r.minFreeBytes), dF, dL, dM);
    }
    Serial.printf("%s cycle done: %ld/%ld cycles, max|d_free|=%ld max|d_largest|=%ld cycles_with_diff=%ld not_running=%ld "
                  "(baseline free=%u largest=%u min_free=%u)\n",
                  kTag, done, n, maxAbsFree, maxAbsLargest, withDiff, inputMismatch, static_cast<unsigned>(base.freeBytes),
                  static_cast<unsigned>(base.largestBlock), static_cast<unsigned>(base.minFreeBytes));
}

}  // namespace

bool isModeCommand(const char* line) {
    const char* rest = nullptr;
    return line != nullptr && word(line, "usb.mode", &rest);
}

void handleConsoleLine(const char* line) {
    const char* rest = nullptr;
    if (!word(line, "usb.mode", &rest)) {
        Serial.printf("%s unknown command\n", kTag);
        return;
    }
    const char* arg = nullptr;
    if (word(rest, "enter", &arg)) {
        const char* why = nullptr;
        const bool ok = requestEnter(&why);
        Serial.printf("%s enter: %s%s%s (watch [USBM] lines; usb.mode stat)\n", kTag, ok ? "accepted" : "REFUSED",
                      why != nullptr ? " - " : "", why != nullptr ? why : "");
    } else if (word(rest, "exit", &arg)) {
        const bool ok = requestExit(ExitCause::User);
        Serial.printf("%s exit: %s (watch [USBM] lines; usb.mode stat)\n", kTag, ok ? "accepted / harmless" : "REFUSED");
    } else if (word(rest, "stat", &arg)) {
        cmdStat();
    } else if (word(rest, "cycle", &arg)) {
        cmdCycle(arg);
    } else {
        Serial.printf("%s usage: usb.mode enter | usb.mode exit | usb.mode stat | usb.mode cycle <N 1..%u>\n", kTag,
                      static_cast<unsigned>(usb_mode_cfg::kCycleMaxCount));
    }
}
#endif  // USB_HOST_DIAG

}  // namespace usb_mode

#endif  // FEATURE_USB_MODE
