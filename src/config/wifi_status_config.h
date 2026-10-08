#pragma once

// Константи передачі рівня сигналу Wi-Fi з AppController до UI (Prompt 36).
// Пороги RSSI -> «рисок» іконки й гістерезис — у config/screens_config.h (це питання відображення);
// тут лише квантування, яке не дає AppState змінюватись від кожного коливання на 1 дБ.

#include <stdint.h>

namespace wifi_status_cfg {

// Крок квантування RSSI у AppState.wifiRssi, дБ (значення кратні кроку).
constexpr int8_t kRssiQuantDb = 2;
// Нижня межа, до якої обрізається RSSI перед квантуванням, дБм.
constexpr int8_t kRssiFloorDb = -100;

static_assert(kRssiQuantDb >= 1, "kRssiQuantDb must be >= 1");
static_assert(kRssiFloorDb < 0, "kRssiFloorDb must be negative");

}  // namespace wifi_status_cfg
