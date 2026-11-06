// [USB13] Діагностика пам'яті (див. usb_memdiag.h). Лише USB_HOST_DIAG = 1.

#include "usb/usb_memdiag.h"

#if FEATURE_USB_HOST && USB_HOST_DIAG

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "config/usb_host_config.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"  // esp_rom_printf: без malloc і без блокувань (безпечно всередині хука невдалого виділення)
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace usb_memdiag {

namespace {

constexpr const char* kTag = "[MEM]";

ClassInfo classInfo(uint32_t caps) {
    multi_heap_info_t i;
    memset(&i, 0, sizeof(i));
    heap_caps_get_info(&i, caps);
    ClassInfo c;
    c.freeBytes = static_cast<uint32_t>(i.total_free_bytes);
    c.largest = static_cast<uint32_t>(i.largest_free_block);
    c.minFree = static_cast<uint32_t>(i.minimum_free_bytes);
    c.freeBlocks = static_cast<uint32_t>(i.free_blocks);
    c.allocBlocks = static_cast<uint32_t>(i.allocated_blocks);
    c.allocBytes = static_cast<uint32_t>(i.total_allocated_bytes);
    return c;
}

void printClass(const char* name, const ClassInfo& c, char* out, size_t cap, size_t* off) {
    const int n = snprintf(out + *off, cap - *off, " | %s free=%u largest=%u min=%u fblk=%u ablk=%u abytes=%u", name,
                           static_cast<unsigned>(c.freeBytes), static_cast<unsigned>(c.largest),
                           static_cast<unsigned>(c.minFree), static_cast<unsigned>(c.freeBlocks),
                           static_cast<unsigned>(c.allocBlocks), static_cast<unsigned>(c.allocBytes));
    if (n > 0) {
        *off += static_cast<size_t>(n);
        if (*off >= cap) {
            *off = cap - 1;
        }
    }
}

// --- Хук невдалих виділень -----------------------------------------------------------------------------------------
struct FailRec {
    uint32_t size;
    uint32_t caps;
    const char* fn;  // статичний рядок з IDF (__func__) або nullptr
    uint32_t tMs;
};

constexpr uint32_t kRing = 8;
FailRec s_ring[kRing];
std::atomic<uint32_t> s_failTotal{0};
std::atomic<uint32_t> s_failInternal{0};
std::atomic<uint32_t> s_failPsram{0};
std::atomic<bool> s_hookInstalled{false};

void onAllocFailed(size_t size, uint32_t caps, const char* functionName) {
    const uint32_t n = s_failTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((caps & MALLOC_CAP_SPIRAM) != 0) {
        s_failPsram.fetch_add(1, std::memory_order_relaxed);
    }
    if ((caps & MALLOC_CAP_INTERNAL) != 0) {
        s_failInternal.fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t tMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    FailRec& r = s_ring[(n - 1) % kRing];
    r.size = static_cast<uint32_t>(size);
    r.caps = caps;
    r.fn = functionName;
    r.tMs = tMs;
    if (n <= usb_host_cfg::kMemFailPrintMax) {
        esp_rom_printf("[MEM] ALLOC FAILED #%u t=%u size=%u caps=0x%x fn=%s\n", static_cast<unsigned>(n),
                       static_cast<unsigned>(tMs), static_cast<unsigned>(size), static_cast<unsigned>(caps),
                       functionName != nullptr ? functionName : "?");
    }
}

// Розшифровка caps (лише у printFailStats, у звичайній задачі).
void capsText(uint32_t caps, char* out, size_t cap) {
    out[0] = '\0';
    size_t off = 0;
    auto add = [&](const char* s) {
        const int n = snprintf(out + off, cap - off, "%s%s", off != 0 ? "|" : "", s);
        if (n > 0) {
            off += static_cast<size_t>(n);
            if (off >= cap) {
                off = cap - 1;
            }
        }
    };
    if (caps & MALLOC_CAP_SPIRAM) add("SPIRAM");
    if (caps & MALLOC_CAP_INTERNAL) add("INTERNAL");
    if (caps & MALLOC_CAP_DMA) add("DMA");
    if (caps & MALLOC_CAP_8BIT) add("8BIT");
    if (caps & MALLOC_CAP_32BIT) add("32BIT");
    if (caps & MALLOC_CAP_EXEC) add("EXEC");
    if (off == 0) {
        add("none");
    }
}

}  // namespace

Snap take() {
    Snap s;
    s.tMs = millis();
    s.internal = classInfo(MALLOC_CAP_INTERNAL);
    s.internal8 = classInfo(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.dma = classInfo(MALLOC_CAP_DMA);
    s.psram = classInfo(MALLOC_CAP_SPIRAM);
    s.tasks = static_cast<uint32_t>(uxTaskGetNumberOfTasks());
    return s;
}

void print(const char* label, const Snap& s) {
    char buf[480];
    size_t off = 0;
    const int n = snprintf(buf, sizeof(buf), "%s %s t=%u", kTag, label != nullptr ? label : "?",
                           static_cast<unsigned>(s.tMs));
    off = (n > 0) ? static_cast<size_t>(n) : 0;
    printClass("INT", s.internal, buf, sizeof(buf), &off);
    printClass("I8", s.internal8, buf, sizeof(buf), &off);
    printClass("DMA", s.dma, buf, sizeof(buf), &off);
    printClass("PSRAM", s.psram, buf, sizeof(buf), &off);
    const int m = snprintf(buf + off, sizeof(buf) - off, " | tasks=%u", static_cast<unsigned>(s.tasks));
    (void)m;
    Serial.println(buf);
}

Snap print(const char* label, bool withIntegrity) {
    const Snap s = take();
    print(label, s);
    if (withIntegrity) {
        const uint32_t t0 = millis();
        const bool intOk = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, false);
        const bool psOk = heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false);
        Serial.printf("%s %s integrity: internal=%s psram=%s (check took %u ms)\n", kTag, label != nullptr ? label : "?",
                      intOk ? "OK" : "CORRUPT", psOk ? "OK" : "CORRUPT", static_cast<unsigned>(millis() - t0));
    }
    return s;
}

void printDelta(const char* label, const Snap& a, const Snap& b) {
    auto d = [](uint32_t after, uint32_t before) { return static_cast<long>(after) - static_cast<long>(before); };
    Serial.printf("%s %s delta(after-before): INT free=%ld largest=%ld ablk=%ld abytes=%ld | PSRAM free=%ld largest=%ld "
                  "ablk=%ld abytes=%ld | DMA free=%ld | tasks=%ld\n",
                  kTag, label != nullptr ? label : "?", d(b.internal.freeBytes, a.internal.freeBytes),
                  d(b.internal.largest, a.internal.largest), d(b.internal.allocBlocks, a.internal.allocBlocks),
                  d(b.internal.allocBytes, a.internal.allocBytes), d(b.psram.freeBytes, a.psram.freeBytes),
                  d(b.psram.largest, a.psram.largest), d(b.psram.allocBlocks, a.psram.allocBlocks),
                  d(b.psram.allocBytes, a.psram.allocBytes), d(b.dma.freeBytes, a.dma.freeBytes),
                  d(b.tasks, a.tasks));
}

void installFailHook() {
    bool expected = false;
    if (!s_hookInstalled.compare_exchange_strong(expected, true)) {
        return;
    }
    const esp_err_t err = heap_caps_register_failed_alloc_callback(onAllocFailed);
    Serial.printf("%s failed-alloc hook: %s (0x%x)\n", kTag, err == ESP_OK ? "installed" : "NOT installed",
                  static_cast<unsigned>(err));
    if (err != ESP_OK) {
        s_hookInstalled.store(false);
    }
}

void printFailStats() {
    const uint32_t total = s_failTotal.load();
    Serial.printf("%s failed allocs: total=%u internal=%u psram=%u hook=%s\n", kTag, static_cast<unsigned>(total),
                  static_cast<unsigned>(s_failInternal.load()), static_cast<unsigned>(s_failPsram.load()),
                  s_hookInstalled.load() ? "installed" : "NOT installed");
    const uint32_t shown = total < kRing ? total : kRing;
    for (uint32_t k = 0; k < shown; ++k) {
        // від найновішого до найстарішого
        const uint32_t idx = (total - 1 - k) % kRing;
        const FailRec r = s_ring[idx];
        char caps[48];
        capsText(r.caps, caps, sizeof(caps));
        Serial.printf("%s   fail #%u t=%u size=%u caps=0x%x (%s) fn=%s\n", kTag, static_cast<unsigned>(total - k),
                      static_cast<unsigned>(r.tMs), static_cast<unsigned>(r.size), static_cast<unsigned>(r.caps), caps,
                      r.fn != nullptr ? r.fn : "?");
    }
}

}  // namespace usb_memdiag

#endif  // FEATURE_USB_HOST && USB_HOST_DIAG
