#pragma once

// [DEF5d] Великі буфери mbedtls (>= tls_psram_cfg::kMinBytes) — у PSRAM, дрібні — в internal (DEF-5: два буфери запису TLS
// по 16 717 Б більше не залежать від розкладки внутрішньої купи). Механізм: mbedtls_platform_set_calloc_free(tlsCalloc,
// tlsFree), викликається ОДИН раз на початку setup() (tls_psram::install()), до WiFi і будь-якого TLS.
//
// Політика (спільна для tlsCalloc і пробних виділень ssl_preflight):
//   розмір >= kMinBytes -> heap_caps (SPIRAM|8BIT); якщо не вдалось — запасно INTERNAL|8BIT;
//   розмір <  kMinBytes -> INTERNAL|8BIT (як CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC за замовчуванням).
// Звільнення — heap_caps_free (коректне для будь-якої області, зокрема для блоків, виділених до install()).
// З tlsCalloc / tlsFree нічого не друкується й не береться жодних мʼютексів: лише атомарні лічильники.
//
// TLS_PSRAM = 0 (config/features.h): install() порожній, політика пробних виділень — лише internal.

#include <stddef.h>
#include <stdint.h>

#include "config/features.h"

namespace tls_psram {

enum class Area : uint8_t {
    None = 0,      // не вмістилось ніде (або не пробувалось)
    Psram = 1,
    Internal = 2,
};

const char* areaName(Area a);  // "none" / "psram" / "internal"

// Один раз, на початку setup() (після Serial.begin: друкує один рядок [TLS]). Повторні виклики ігноруються.
void install();

// Пробне виділення за політикою вище (heap_caps_malloc, без обнулення; лічильники НЕ чіпає). keep != nullptr: блок
// віддається викликачу (він звільнить heap_caps_free; потрібно, щоб тримати кілька проб одночасно); keep == nullptr:
// блок звільняється одразу. Повертає область, з якої пройшла проба, або Area::None.
Area probeAlloc(size_t bytes, void** keep);

#if FEATURE_USB_HOST && USB_HOST_DIAG
// Консоль: `usb.tls [stat]` — див. usb_play_diag.cpp. [CLEAN1] Псевдонім `tls.mem` прибрано: консольний диспетчер пропускає
// лише рядки, що починаються на «u», тож `tls.mem` ніколи не доходив до обробника.
void handleConsole(const char* arg);
#endif

}  // namespace tls_psram

