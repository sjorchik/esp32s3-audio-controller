// [DEF5b] Запобіжна перевірка перед https-підключенням радіо після USB-сесії (опис, інваріанти й захист від петлі —
// у core/ssl_preflight.h). Лише WORKAROUND для DEF-5: причину (власник блока 448 Б) не усуває.
// [DEF5d] ЗМІНЕНО: пробні виділення повторюють політику tls_psram (>= kMinBytes — спершу PSRAM, потім internal; див.
// tls_psram::probeAlloc), у логу показано область. Логіка очікування, перезапуску, обмежувача й force НЕ змінена.

#include "core/ssl_preflight.h"

#include "config/ssl_preflight_config.h"

#if SSL_PREFLIGHT && FEATURE_USB_HOST

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <atomic>

#include "core/app_controller.h"
#include "core/tls_psram.h"  // [DEF5d] probeAlloc / areaName
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ssl_preflight {

namespace {

constexpr const char* kTag = "[PREFLIGHT]";

std::atomic<bool> s_usbSession{false};       // у цьому запуску була хоч одна USB-сесія (скидається перезапуском)
std::atomic<bool> s_restartRequested{false}; // перезапуск за перевіркою уже замовлено (один за запуск)
std::atomic<bool> s_force{false};            // консоль `usb.preflight force on`: перевірка штучно «не вміщається»

struct Result {
    bool ok = false;
    uint8_t okCount = 0;    // скільки пробних виділень вдалось у ОСТАННІЙ спробі
    uint32_t tries = 0;     // скільки спроб (1-based)
    uint32_t waitedMs = 0;  // від першої спроби до кінця
    bool forced = false;
    tls_psram::Area area[ssl_preflight_cfg::kProbeCount] = {};  // [DEF5d] звідки пройшла кожна проба останньої спроби
};

bool httpsUrl(const char* url) { return url != nullptr && strncasecmp(url, "https://", 8) == 0; }

uint32_t freeInternal() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t largestInternal() { return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)); }

// Одна проба: kProbeCount послідовних виділень по kTlsBufBytes (перше невдале зупиняє серію), потім звільнення ВСІХ
// успішних. Повертає, скільки виділень вдалось. [DEF5d] Виділення — за політикою tls_psram (PSRAM, потім internal);
// areas[i] — область i-ї проби (None — не вмістилось або не пробувалась).
uint8_t probeOnce(tls_psram::Area* areas) {
    void* blk[ssl_preflight_cfg::kProbeCount] = {};
    uint8_t n = 0;
    for (uint8_t i = 0; i < ssl_preflight_cfg::kProbeCount; ++i) {
        areas[i] = tls_psram::Area::None;
    }
    for (uint8_t i = 0; i < ssl_preflight_cfg::kProbeCount; ++i) {
        areas[i] = tls_psram::probeAlloc(ssl_preflight_cfg::kTlsBufBytes, &blk[i]);
        if (areas[i] == tls_psram::Area::None) {
            break;
        }
        ++n;
    }
    for (uint8_t i = 0; i < n; ++i) {
        heap_caps_free(blk[i]);
    }
    return n;
}

// Перевірка з очікуванням: повтор кожні kStepMs до kMaxWaitMs від першої спроби (відкладені звільнення задач
// можуть встигнути). Блокує викликача до kMaxWaitMs — викликати лише з задачі, якій це дозволено.
Result evaluate() {
    Result r;
    r.forced = s_force.load();
    const uint32_t t0 = millis();
    for (;;) {
        ++r.tries;
        r.okCount = r.forced ? 0 : probeOnce(r.area);
        if (r.okCount == ssl_preflight_cfg::kProbeCount) {
            r.ok = true;
            break;
        }
        if (static_cast<uint32_t>(millis() - t0) >= ssl_preflight_cfg::kMaxWaitMs) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ssl_preflight_cfg::kStepMs));
    }
    r.waitedMs = static_cast<uint32_t>(millis() - t0);
    return r;
}

// «alloc#1 ok(psram), alloc#2 FAIL» (після першої невдачі решта — «skipped»; при force — «FORCED»). [DEF5d] у дужках — область.
void describeAllocs(const Result& r, char* out, size_t cap) {
    size_t pos = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < ssl_preflight_cfg::kProbeCount; ++i) {
        const bool good = !r.forced && i < r.okCount;
        const char* s = r.forced ? "FORCED" : (good ? "ok" : (i == r.okCount ? "FAIL" : "skipped"));
        const int w = snprintf(out + pos, cap - pos, "%salloc#%u %s%s%s%s", i == 0 ? "" : ", ",
                               static_cast<unsigned>(i + 1), s, good ? "(" : "", good ? tls_psram::areaName(r.area[i]) : "",
                               good ? ")" : "");
        if (w < 0) {
            break;
        }
        pos += static_cast<size_t>(w);
        if (pos >= cap) {
            break;
        }
    }
}

void logSkip(const char* why) {
#if USB_HOST_DIAG  // у релізі пропуски не логуються (http-станції переконнектяться часто)
    Serial.printf("%s skipped: %s\n", kTag, why);
#else
    (void)why;
#endif
}

#if !USB_HEAPMAP  // під USB_HEAPMAP = 1 перезапуску немає (див. beforeConnect)
// Одноразова задача: тихий перезапуск контролера -> ESP.restart(). prepareForRestart() блокує викликача (до
// kPrepareRestartWaitMs), тому НЕ з задачі плеєра (вона мусить далі обробити Cmd::Stop від beginRestart()).
// На відміну від ssl_workaround, RTC-прапорця «блок після власного перезапуску» не ставимо (рішення планування:
// після перезапуску прапорець USB-сесії скинутий, тож цикл неможливий).
void restartTask(void*) {
    const bool prepared = AppController::prepareForRestart();
    Serial.printf("%s quiet restart prepared=%d (false = timeout / controller busy: restarting anyway)\n", kTag,
                  prepared ? 1 : 0);
#if USB_HOST_DIAG
    Serial.printf("%s restart task stack min free=%u B\n", kTag,
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    Serial.printf("%s restarting now\n", kTag);
    Serial.flush();
    ESP.restart();
    vTaskDelete(nullptr);  // недосяжно
}
#endif

}  // namespace

void noteUsbSession() {
    if (!s_usbSession.exchange(true)) {
        Serial.printf("%s USB session noted: https connects are checked until reboot\n", kTag);
    }
}

Verdict beforeConnect(const char* url) {
    if (!httpsUrl(url)) {  // TLS-буферів нема — перевірка не потрібна
        logSkip("http station");
        return Verdict::Proceed;
    }
    if (!s_usbSession.load()) {
        logSkip("no USB session since boot");
        return Verdict::Proceed;
    }
    if (s_restartRequested.load()) {  // перезапуск уже в дорозі (наприклад, повтор після паузи): не підключатись
        return Verdict::RestartRequested;
    }

    const Result r = evaluate();
    char allocs[96];
    describeAllocs(r, allocs, sizeof(allocs));
    const unsigned freeB = static_cast<unsigned>(freeInternal());
    const unsigned largestB = static_cast<unsigned>(largestInternal());

    if (r.ok) {
        Serial.printf("%s https connect: ok (try %u, waited %u ms) free=%u largest=%u\n", kTag,
                      static_cast<unsigned>(r.tries), static_cast<unsigned>(r.waitedMs), freeB, largestB);
        return Verdict::Proceed;
    }

#if USB_HEAPMAP
    // [DEF5a] Діагностика DEF5c: збій має лишитись видимим, тому перезапуску немає, підключення йде як зазвичай.
    Serial.printf("%s NO-FIT after %u ms (%s) free=%u largest=%u -> would restart now, SUPPRESSED by USB_HEAPMAP=1, "
                  "connecting anyway\n",
                  kTag, static_cast<unsigned>(r.waitedMs), allocs, freeB, largestB);
    return Verdict::Proceed;
#else
    const uint32_t uptime = millis();
    if (uptime < ssl_preflight_cfg::kMinUptimeMs) {
        Serial.printf("%s NO-FIT after %u ms (%s) free=%u largest=%u -> restart rate-limited (uptime %u ms < %u ms), "
                      "connecting anyway\n",
                      kTag, static_cast<unsigned>(r.waitedMs), allocs, freeB, largestB, static_cast<unsigned>(uptime),
                      static_cast<unsigned>(ssl_preflight_cfg::kMinUptimeMs));
        return Verdict::Proceed;
    }
    bool expected = false;
    if (!s_restartRequested.compare_exchange_strong(expected, true)) {
        return Verdict::RestartRequested;  // гонка з іншим викликом: перезапуск уже замовлено
    }
    Serial.printf("%s NO-FIT after %u ms (%s) free=%u largest=%u -> quiet restart\n", kTag,
                  static_cast<unsigned>(r.waitedMs), allocs, freeB, largestB);
    const BaseType_t ok = xTaskCreatePinnedToCore(restartTask, "pf_restart", ssl_preflight_cfg::kRestartTaskStack,
                                                  nullptr, ssl_preflight_cfg::kRestartTaskPriority, nullptr,
                                                  ssl_preflight_cfg::kRestartTaskCore);
    if (ok != pdPASS) {
        Serial.printf("%s ERROR: restart task create failed - connecting anyway\n", kTag);
        s_restartRequested.store(false);
        return Verdict::Proceed;
    }
    return Verdict::RestartRequested;
#endif
}

#if USB_HOST_DIAG
void handleConsole(const char* arg) {
    const char* a = (arg != nullptr) ? arg : "";
    if (strcmp(a, "test") == 0) {
        // Та сама перевірка з очікуванням, що й перед підключенням, але БЕЗ умов (https / USB-сесія) і БЕЗ перезапуску.
        const Result r = evaluate();
        char allocs[96];
        describeAllocs(r, allocs, sizeof(allocs));
        Serial.printf("%s test: %s (try %u, waited %u ms; %s) free=%u largest=%u force=%d\n", kTag,
                      r.ok ? "ok" : "NO-FIT", static_cast<unsigned>(r.tries), static_cast<unsigned>(r.waitedMs), allocs,
                      static_cast<unsigned>(freeInternal()), static_cast<unsigned>(largestInternal()),
                      s_force.load() ? 1 : 0);
        return;
    }
    if (strncmp(a, "force", 5) == 0) {
        const char* v = a + 5;
        while (*v == ' ' || *v == '\t') {
            ++v;
        }
        if (strcmp(v, "on") == 0) {
            s_force.store(true);
            Serial.printf("%s force ON: checks report NO-FIT until `usb.preflight force off` or reboot\n", kTag);
            return;
        }
        if (strcmp(v, "off") == 0) {
            s_force.store(false);
            Serial.printf("%s force OFF\n", kTag);
            return;
        }
    }
    if (a[0] != '\0' && strcmp(a, "stat") != 0) {  // невідомий аргумент або «force» без on / off
        Serial.printf("%s usage: usb.preflight test | force on | force off | stat\n", kTag);
        return;
    }
    Serial.printf("%s stat: usb_session=%d restart_requested=%d force=%d uptime=%u ms min_uptime=%u ms free=%u largest=%u "
                  "probe=%ux%u B wait<=%u ms step=%u ms heapmap=%d tls_psram=%d\n",
                  kTag, s_usbSession.load() ? 1 : 0, s_restartRequested.load() ? 1 : 0, s_force.load() ? 1 : 0,
                  static_cast<unsigned>(millis()), static_cast<unsigned>(ssl_preflight_cfg::kMinUptimeMs),
                  static_cast<unsigned>(freeInternal()), static_cast<unsigned>(largestInternal()),
                  static_cast<unsigned>(ssl_preflight_cfg::kProbeCount),
                  static_cast<unsigned>(ssl_preflight_cfg::kTlsBufBytes),
                  static_cast<unsigned>(ssl_preflight_cfg::kMaxWaitMs), static_cast<unsigned>(ssl_preflight_cfg::kStepMs),
                  USB_HEAPMAP ? 1 : 0, TLS_PSRAM ? 1 : 0);
}
#endif  // USB_HOST_DIAG

}  // namespace ssl_preflight

#endif  // SSL_PREFLIGHT && FEATURE_USB_HOST

