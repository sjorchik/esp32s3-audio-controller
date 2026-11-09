#pragma once

// [DEF5d] Константи розміщення буферів mbedtls у PSRAM (core/tls_psram.*). Окремий файл за правилом 4 спеки.

#include <stddef.h>
#include <stdint.h>

namespace tls_psram_cfg {

// Виділення mbedtls розміром >= kMinBytes йдуть у PSRAM (запасно — internal), менші — у internal, як у
// CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC. 16 384 = CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN: буфери запису TLS (≈ 16 717 Б, DEF-5) —
// єдині виділення такого розміру; усе дрібніше лишається в internal. Не змінювати без потреби.
constexpr size_t kMinBytes = 16384;

// Скільки перших великих виділень запамʼятати (розмір + область) для друку командою `tls.mem stat`.
constexpr uint8_t kLogFirstBigAllocs = 8;

static_assert(kMinBytes >= 4096 && kMinBytes <= 65536, "DEF5d: sane PSRAM threshold");
static_assert(kLogFirstBigAllocs >= 1 && kLogFirstBigAllocs <= 32, "DEF5d: big-alloc log size 1..32");

}  // namespace tls_psram_cfg
