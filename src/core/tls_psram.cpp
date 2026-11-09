// [DEF5d] Буфери mbedtls >= tls_psram_cfg::kMinBytes — у PSRAM (опис, політика й інваріанти — у core/tls_psram.h).

#include "core/tls_psram.h"

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "config/ssl_preflight_config.h"
#include "config/tls_psram_config.h"
#include "esp_heap_caps.h"
#if TLS_PSRAM
#include "mbedtls/platform.h"  // mbedtls_platform_set_calloc_free (MBEDTLS_PLATFORM_MEMORY; ГІПОТЕЗА: оголошений у цій збірці)
#endif

// Розмір проби ssl_preflight (16 717 Б) має підпадати під політику PSRAM, інакше проби не відображають реальні виділення.
static_assert(tls_psram_cfg::kMinBytes <= ssl_preflight_cfg::kTlsBufBytes, "DEF5d: TLS write buffer must be >= kMinBytes");

namespace tls_psram {

namespace {

constexpr const char* kTag = "[TLS]";
constexpr uint32_t kCapsPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t kCapsInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

std::atomic<bool> s_installed{false};
std::atomic<int> s_installRc{-1};  // результат mbedtls_platform_set_calloc_free (0 = ок); -1 — install() не викликався / TLS_PSRAM = 0
std::atomic<uint32_t> s_bigPsram{0};     // великі виділення, що пройшли в PSRAM
std::atomic<uint32_t> s_bigFallback{0};  // великі виділення, що не вмістились у PSRAM і пройшли в internal
std::atomic<uint32_t> s_bigFailed{0};    // великі виділення, що не вдались ніде
std::atomic<uint32_t> s_smallInternal{0};
std::atomic<uint32_t> s_frees{0};
std::atomic<uint32_t> s_lastSize{0};     // розмір (n * size) останнього запиту
std::atomic<uint32_t> s_bigSeq{0};       // скільки великих запитів було всього (індекс у журналі)
std::atomic<uint32_t> s_bigLog[tls_psram_cfg::kLogFirstBigAllocs];  // розмір (молодші 30 біт) | область << 30; 0 — порожньо

bool wantPsram(size_t bytes) {
#if TLS_PSRAM
    return bytes >= tls_psram_cfg::kMinBytes;
#else
    (void)bytes;
    return false;
#endif
}

#if TLS_PSRAM
void noteBig(size_t total, Area area) {
    const uint32_t idx = s_bigSeq.fetch_add(1, kRelaxed);
    if (idx < tls_psram_cfg::kLogFirstBigAllocs) {
        const uint32_t sz = total > 0x3FFFFFFFu ? 0x3FFFFFFFu : static_cast<uint32_t>(total);
        s_bigLog[idx].store(sz | (static_cast<uint32_t>(area) << 30), kRelaxed);
    }
}

// Викликаються з mbedtls (будь-яка задача, зокрема з-під мʼютексів і з IRAM-безпечних шляхів не очікується): без друку, без
// мʼютексів, лише атомарні лічильники.
void* tlsCalloc(size_t n, size_t size) {
    if (n != 0 && size > SIZE_MAX / n) {  // переповнення n * size
        return nullptr;
    }
    const size_t total = n * size;
    s_lastSize.store(static_cast<uint32_t>(total), kRelaxed);
    if (total >= tls_psram_cfg::kMinBytes) {
        void* p = heap_caps_calloc(n, size, kCapsPsram);
        if (p != nullptr) {
            s_bigPsram.fetch_add(1, kRelaxed);
            noteBig(total, Area::Psram);
            return p;
        }
        p = heap_caps_calloc(n, size, kCapsInternal);  // запасний шлях: PSRAM не вмістив
        if (p != nullptr) {
            s_bigFallback.fetch_add(1, kRelaxed);
            noteBig(total, Area::Internal);
            return p;
        }
        s_bigFailed.fetch_add(1, kRelaxed);
        noteBig(total, Area::None);
        return nullptr;
    }
    s_smallInternal.fetch_add(1, kRelaxed);
    return heap_caps_calloc(n, size, kCapsInternal);
}

void tlsFree(void* p) {
    s_frees.fetch_add(1, kRelaxed);
    heap_caps_free(p);  // працює для будь-якої області, зокрема для блоків, виділених до install()
}
#endif  // TLS_PSRAM

}  // namespace

const char* areaName(Area a) {
    switch (a) {
        case Area::Psram:
            return "psram";
        case Area::Internal:
            return "internal";
        default:
            return "none";
    }
}

void install() {
#if TLS_PSRAM
    if (s_installed.exchange(true)) {
        return;
    }
    const int rc = mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
    s_installRc.store(rc, kRelaxed);
    Serial.printf("%s mbedtls allocator %s (rc=%d): blocks >= %u B -> PSRAM (fallback internal), smaller -> internal\n", kTag,
                  rc == 0 ? "installed" : "NOT installed", rc, static_cast<unsigned>(tls_psram_cfg::kMinBytes));
#endif
}

Area probeAlloc(size_t bytes, void** keep) {
    void* p = nullptr;
    Area a = Area::None;
    if (wantPsram(bytes)) {
        p = heap_caps_malloc(bytes, kCapsPsram);
        if (p != nullptr) {
            a = Area::Psram;
        }
    }
    if (p == nullptr) {
        p = heap_caps_malloc(bytes, kCapsInternal);
        if (p != nullptr) {
            a = Area::Internal;
        }
    }
    if (keep != nullptr) {
        *keep = p;
    } else if (p != nullptr) {
        heap_caps_free(p);
    }
    return a;
}

#if FEATURE_USB_HOST && USB_HOST_DIAG
void handleConsole(const char* arg) {
    const char* a = (arg != nullptr) ? arg : "";
    if (a[0] != '\0' && strcmp(a, "stat") != 0) {
        Serial.printf("%s usage: tls.mem [stat]   (alias: usb.tls [stat])\n", kTag);
        return;
    }
    Serial.printf("%s mem stat: TLS_PSRAM=%d installed=%d rc=%d kMinBytes=%u bigPsram=%lu fallback=%lu bigFailed=%lu "
                  "smallInternal=%lu frees=%lu lastSize=%lu\n",
                  kTag, TLS_PSRAM ? 1 : 0, s_installed.load() ? 1 : 0, s_installRc.load(),
                  static_cast<unsigned>(tls_psram_cfg::kMinBytes), static_cast<unsigned long>(s_bigPsram.load()),
                  static_cast<unsigned long>(s_bigFallback.load()), static_cast<unsigned long>(s_bigFailed.load()),
                  static_cast<unsigned long>(s_smallInternal.load()), static_cast<unsigned long>(s_frees.load()),
                  static_cast<unsigned long>(s_lastSize.load()));
    Serial.printf("%s mem stat: psram_free=%u internal free=%u largest=%u\n", kTag,
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    const uint32_t seq = s_bigSeq.load();
    for (uint32_t i = 0; i < tls_psram_cfg::kLogFirstBigAllocs && i < seq; ++i) {
        const uint32_t v = s_bigLog[i].load();
        Serial.printf("%s mem stat: big#%lu size=%lu area=%s\n", kTag, static_cast<unsigned long>(i),
                      static_cast<unsigned long>(v & 0x3FFFFFFFu), areaName(static_cast<Area>(v >> 30)));
    }
    if (seq > tls_psram_cfg::kLogFirstBigAllocs) {
        Serial.printf("%s mem stat: ... %lu more big allocations not listed\n", kTag,
                      static_cast<unsigned long>(seq - tls_psram_cfg::kLogFirstBigAllocs));
    }
}
#endif  // FEATURE_USB_HOST && USB_HOST_DIAG

}  // namespace tls_psram
