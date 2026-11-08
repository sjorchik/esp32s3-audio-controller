// [DEF5a] При USB_HEAPMAP = 1 trigger() НЕ перезапускає плату (лише лог «would restart»), щоб збій не стирав стан кучі.
// [USB13b] WORKAROUND для відкритого DEF-5 (див. ssl_workaround.h). Усі збірки (захист для користувача, не діагностика);
// вимикається usb_host_cfg::kRestartOnSslAllocFail = false. Для інших помилок радіо поведінка не змінюється.

#include "core/ssl_workaround.h"

#include "config/usb_host_config.h"

#if FEATURE_USB_HOST

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <atomic>

#include "core/app_controller.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if FEATURE_USB_MODE
#include "usb/usb_mode.h"
#endif
#if USB_HOST_DIAG
#include "usb/usb_memdiag.h"
#endif

namespace ssl_workaround {

namespace {

constexpr const char* kTag = "[WORKAROUND]";

// RTC_NOINIT: переживає програмний перезапуск (ESP.restart), після подачі живлення — випадкове сміття; тому
// чинним прапорець вважається лише за magic + інверсією І esp_reset_reason() == ESP_RST_SW.
constexpr uint32_t kRtcMagic = 0x55B13B5Au;
RTC_NOINIT_ATTR uint32_t s_rtcMagic;
RTC_NOINIT_ATTR uint32_t s_rtcInv;

std::atomic<bool> s_blocked{false};    // попередній reset спричинив цей модуль -> обхід вимкнено на весь запуск
std::atomic<bool> s_triggered{false};  // уже ініційовано перезапуск у цьому запуску
std::atomic<bool> s_filePlayed{false}; // у поточному сеансі режиму «USB» відкривався файл
std::atomic<bool> s_armed{false};      // вікно після виходу чинне
std::atomic<uint32_t> s_armedAtMs{0};  // millis() моменту виходу (початок вікна)
std::atomic<uint32_t> s_fails{0};      // https-невдач поспіль у вікні
std::atomic<uint32_t> s_libText{0};    // скільки разів бібліотека переслала текст про помилку виділення SSL
std::atomic<bool> s_expiredLogged{false};

bool httpsUrl(const char* url) { return url != nullptr && strncasecmp(url, "https://", 8) == 0; }

uint32_t sinceExitMs() { return static_cast<uint32_t>(millis() - s_armedAtMs.load()); }

// Одноразова задача: знімок пам'яті (DIAG) -> тихий перезапуск контролера -> RTC-прапорець -> ESP.restart().
// prepareForRestart() блокує викликача (до kPrepareRestartWaitMs), тому викликається саме з окремої задачі.
void restartTask(void*) {
#if USB_HOST_DIAG
    usb_memdiag::print("workaround:before restart", true);
    usb_memdiag::printFailStats();
#endif
    const bool prepared = AppController::prepareForRestart();
    Serial.printf("%s quiet restart prepared=%d (false = timeout / controller busy: restarting anyway)\n", kTag,
                  prepared ? 1 : 0);
    // Прапорець — ПЕРЕД самим перезапуском (якщо перезапуск не відбудеться, застарілий прапорець не лишиться).
    s_rtcMagic = kRtcMagic;
    s_rtcInv = ~kRtcMagic;
#if USB_HOST_DIAG
    Serial.printf("%s restart task stack min free=%u B\n", kTag,
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    Serial.printf("%s restarting now\n", kTag);
    Serial.flush();
    ESP.restart();
    vTaskDelete(nullptr);  // недосяжно
}

// true — рішення прийнято (у dry — «було б прийнято»).
bool trigger(uint32_t failCount, uint32_t sinceMs, bool simulated, bool dry) {
    if (s_blocked.load()) {
        Serial.printf("%s SUPPRESSED: previous reset was caused by this workaround - no second restart in this run\n",
                      kTag);
        return false;
    }
#if USB_HEAPMAP
    // [DEF5a] kRestartOnSslAllocFail ПРИМУСОВО «false» для самого перезапуску: стан кучі після збою потрібен для знімка.
    if (!dry) {
        Serial.printf("%s would restart now (fails=%u since_exit=%u ms%s) - SUPPRESSED by USB_HEAPMAP=1: take `usb.hm dump full`, then reboot manually\n",
                      kTag, static_cast<unsigned>(failCount), static_cast<unsigned>(sinceMs), simulated ? ", simulated" : "");
        return false;
    }
#endif
    if (dry) {
        Serial.printf("%s DRY RUN: would restart now (fails=%u since_exit=%u ms%s) - nothing done\n", kTag,
                      static_cast<unsigned>(failCount), static_cast<unsigned>(sinceMs), simulated ? ", simulated" : "");
        return true;
    }
    bool expected = false;
    if (!s_triggered.compare_exchange_strong(expected, true)) {
        Serial.printf("%s SUPPRESSED: restart already initiated in this run\n", kTag);
        return false;
    }
    s_armed.store(false);
    Serial.printf("%s SSL alloc failure suspected after a USB session with a file: radio connect failed %ux in %u ms "
                  "after exit (window %u ms)%s -> one-time quiet restart. This is a WORKAROUND for the open DEF-5, "
                  "not a fix (lib_text_seen=%u)\n",
                  kTag, static_cast<unsigned>(failCount), static_cast<unsigned>(sinceMs),
                  static_cast<unsigned>(usb_host_cfg::kRestartWindowMs), simulated ? " [SIMULATED]" : "",
                  static_cast<unsigned>(s_libText.load()));
    const BaseType_t ok = xTaskCreatePinnedToCore(restartTask, "ssl_restart", usb_host_cfg::kRestartTaskStack, nullptr,
                                                  usb_host_cfg::kRestartTaskPriority, nullptr,
                                                  usb_host_cfg::kRestartTaskCore);
    if (ok != pdPASS) {
        Serial.printf("%s ERROR: restart task create failed - no restart\n", kTag);
        s_triggered.store(false);
        return false;
    }
    return true;
}

// Облік однієї невдачі підключення. simulated = true (консоль): без перевірок https / usb_mode / Wi-Fi.
void registerFailure(const char* url, bool simulated, bool dry) {
    if (!usb_host_cfg::kRestartOnSslAllocFail) {
        return;
    }
    if (!s_armed.load()) {
        if (simulated) {
            Serial.printf("%s simulate: not armed - nothing to do\n", kTag);
        }
        return;
    }
    const uint32_t since = sinceExitMs();
    if (since > usb_host_cfg::kRestartWindowMs) {
        s_armed.store(false);
        if (!s_expiredLogged.exchange(true) || simulated) {
            Serial.printf("%s window expired (%u ms > %u ms since USB exit): failure not counted, workaround disarmed\n",
                          kTag, static_cast<unsigned>(since), static_cast<unsigned>(usb_host_cfg::kRestartWindowMs));
        }
        return;
    }
    if (!simulated) {
#if FEATURE_USB_MODE
        {  // режим «USB» знову активний (вхід / готовий / грає): радіо не очікується. Exiting / Off / Error НЕ відсіюємо:
           // перша спроба радіо починається ще під час Exiting (відновлення входу у runExit), до setState(Off).
            const usb_mode::State st = usb_mode::state();
            if (st == usb_mode::State::Entering || st == usb_mode::State::Ready || st == usb_mode::State::Playing) {
                return;
            }
        }
#endif
        if (!httpsUrl(url)) {  // помилка виділення SSL можлива лише для https
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {  // немає мережі — це не наш випадок
            Serial.printf("%s radio connect failed but Wi-Fi is not connected - not counted\n", kTag);
            return;
        }
    }
    const uint32_t n = s_fails.fetch_add(1) + 1;
    Serial.printf("%s radio connect failed after USB file session: %u/%u (%u ms after exit)\n", kTag,
                  static_cast<unsigned>(n), static_cast<unsigned>(usb_host_cfg::kRestartFailCount),
                  static_cast<unsigned>(since));
    if (n >= usb_host_cfg::kRestartFailCount) {
        trigger(n, since, simulated, dry);
    }
}

}  // namespace

void onBoot() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool flag = (s_rtcMagic == kRtcMagic && s_rtcInv == ~kRtcMagic);
    s_rtcMagic = 0;  // прапорець діє лише на ОДИН запуск
    s_rtcInv = 0;
    if (flag && reason == ESP_RST_SW) {
        s_blocked.store(true);
        Serial.printf("%s boot: previous reset was caused by the SSL workaround -> workaround DISABLED for this run\n",
                      kTag);
    } else if (flag) {
        Serial.printf("%s boot: stale RTC flag ignored (reset reason %d)\n", kTag, static_cast<int>(reason));
    }
#if USB_HOST_DIAG
    Serial.printf("%s boot: reset reason %d, enabled=%d (window %u ms, fails %u)\n", kTag, static_cast<int>(reason),
                  usb_host_cfg::kRestartOnSslAllocFail ? 1 : 0, static_cast<unsigned>(usb_host_cfg::kRestartWindowMs),
                  static_cast<unsigned>(usb_host_cfg::kRestartFailCount));
#endif
}

void noteFilePlayed() { s_filePlayed.store(true); }

void noteUsbExit() {
    const bool played = s_filePlayed.exchange(false);
    s_fails.store(0);
    s_expiredLogged.store(false);
    if (!played) {
        s_armed.store(false);
        return;
    }
    s_armedAtMs.store(millis());
    s_armed.store(true);
    Serial.printf("%s USB exit after a session with a file: armed for %u ms (enabled=%d, blocked=%d)\n", kTag,
                  static_cast<unsigned>(usb_host_cfg::kRestartWindowMs), usb_host_cfg::kRestartOnSslAllocFail ? 1 : 0,
                  s_blocked.load() ? 1 : 0);
}

void onRadioConnect(bool ok, const char* url) {
    if (ok) {
        s_fails.store(0);  // «поспіль»: успішне підключення обриває серію
        return;
    }
    registerFailure(url, false, false);
}

void noteLibSslAllocText() {
    const uint32_t n = s_libText.fetch_add(1) + 1;
    if (n <= 3) {
        Serial.printf("%s library reported an SSL allocation error text (#%u)\n", kTag, static_cast<unsigned>(n));
    }
}

#if USB_HOST_DIAG
void handleConsole(const char* arg) {
    const bool dry = (arg != nullptr && strcmp(arg, "dry") == 0);
    const bool expired = (arg != nullptr && strcmp(arg, "expired") == 0);
    const bool stat = (arg != nullptr && strcmp(arg, "stat") == 0);
    const bool plain = (arg == nullptr || arg[0] == '\0');
    if (stat || !(plain || dry || expired)) {
        if (!stat) {
            Serial.printf("%s usage: usb.sslfail [dry | expired | stat]\n", kTag);
        }
        Serial.printf("%s stat: enabled=%d blocked=%d triggered=%d armed=%d file_played=%d fails=%u/%u since_exit=%u ms "
                      "window=%u ms lib_text_seen=%u\n",
                      kTag, usb_host_cfg::kRestartOnSslAllocFail ? 1 : 0, s_blocked.load() ? 1 : 0,
                      s_triggered.load() ? 1 : 0, s_armed.load() ? 1 : 0, s_filePlayed.load() ? 1 : 0,
                      static_cast<unsigned>(s_fails.load()), static_cast<unsigned>(usb_host_cfg::kRestartFailCount),
                      s_armed.load() ? static_cast<unsigned>(sinceExitMs()) : 0u,
                      static_cast<unsigned>(usb_host_cfg::kRestartWindowMs), static_cast<unsigned>(s_libText.load()));
        return;
    }
    if (!usb_host_cfg::kRestartOnSslAllocFail) {
        Serial.printf("%s simulate: kRestartOnSslAllocFail = false (disabled by config) - nothing done\n", kTag);
        return;
    }
    // Імітація: ті самі registerFailure() / trigger(), що й у бойовому шляху (без перевірок https / usb_mode / Wi-Fi).
    const bool oldArmed = s_armed.load();
    const uint32_t oldAt = s_armedAtMs.load();
    const uint32_t oldFails = s_fails.load();
    s_armedAtMs.store(millis() - (expired ? usb_host_cfg::kRestartWindowMs + 1000u : 0u));
    s_armed.store(true);
    s_fails.store(usb_host_cfg::kRestartFailCount - 1);  // наступна «невдача» досягає порогу
    s_expiredLogged.store(false);
    Serial.printf("%s simulate: %s%s\n", kTag, dry ? "DRY " : "", expired ? "window already expired" : "event -32512 after USB exit with a file");
    registerFailure("https://simulated.invalid/", true, dry);
    if (dry || expired || s_blocked.load() || USB_HEAPMAP) {  // нічого не ініційовано: повернути попередній стан
        s_armed.store(oldArmed);
        s_armedAtMs.store(oldAt);
        s_fails.store(oldFails);
    }
}
#endif  // USB_HOST_DIAG

}  // namespace ssl_workaround

#else  // !FEATURE_USB_HOST: файлів немає, обхід не потрібен

namespace ssl_workaround {
void onBoot() {}
void noteFilePlayed() {}
void noteUsbExit() {}
void onRadioConnect(bool, const char*) {}
void noteLibSslAllocText() {}
}  // namespace ssl_workaround

#endif  // FEATURE_USB_HOST
