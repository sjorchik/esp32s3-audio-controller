#pragma once

// Значення за замовчуванням без «магічних» чисел у модулях.

#include <stdint.h>

namespace defaults {

// --- Консоль ---
constexpr uint32_t kSerialBaud = 115200;

// --- Звук ---
// [Prompt 48a] I2C та аудіопроцесор (TDA7318 / PT2313L) вирізано.
// [Prompt 50b] kDefaultBass / kDefaultTreble / kDefaultBalance / kDefaultLoudness ВИДАЛЕНО разом із полями
// AppStateData; баланс тепер у dB: balance_cfg::kDefaultDb (config/balance_config.h).
constexpr int8_t kDefaultVolume  = 40;
constexpr bool   kDefaultMute    = false;

// [Prompt 48a] Логічні входи (3): 0 = WiFi Radio (цифровий шлях, реле НЕ задіяне), 1 = TV Box,
// 2 = Computer. Входи 1 / 2 комутує одне реле (audio/input_selector: вимкнено = TV Box, увімкнено =
// Computer). Bluetooth і AUX ВИДАЛЕНО. Назви редагуються у вебі; тут — типові. Усі три входи
// дозволені за замовчуванням (settings_cfg::kInputMaskDefault = 0x07).
constexpr uint8_t kDefaultInput = 0;
constexpr uint8_t kInputCount = 3;
constexpr const char* kInputNames[kInputCount] = {
    "WiFi Radio",
    "TV Box",
    "Computer",
};

// --- Дисплей ---
constexpr uint8_t kDefaultBrightness = 80;

// --- Подієва шина ---
constexpr uint16_t kEventQueueSize = 32;

// --- Wi-Fi provisioning ---
constexpr const char* kApSsid = "AudioCtrl-Setup";
constexpr uint8_t kWifiRetryBeforeAp = 3;

// --- mDNS ---
constexpr const char* kMdnsName = "audio";

// --- LittleFS ---
constexpr const char* kLittleFsMountPoint = "/littlefs";

}  // namespace defaults