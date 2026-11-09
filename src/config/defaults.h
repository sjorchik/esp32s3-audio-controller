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
// [INP4a-1] Слоти НАЛАШТУВАНЬ входів (Settings::inputNames, біти Settings::inputEnabledMask) = логічні входи + «USB»:
// kInputSlotCount = 4, слот kUsbInputIndex = 3. kInputCount (3) НЕ змінено: веб-JSON, AppController (s_input 0..2) і
// player_mode_cfg (kProfileCount = kInputCount + 1 = 4) працюють як раніше. Профіль слота 3 («Player») уже існує.
constexpr uint8_t kInputSlotCount = 4;
constexpr uint8_t kUsbInputIndex = 3;
static_assert(kInputCount < kInputSlotCount && kUsbInputIndex == kInputSlotCount - 1,
              "USB input is the last settings slot, right after the logical inputs");
// Імена за замовчуванням: перші kInputCount елементів — як раніше (читачі, що індексують за kInputCount, не змінились);
// слот 3 — за вказівкою власника «Player» (НЕ «USB Player»).
constexpr const char* kInputNames[kInputSlotCount] = {
    "WiFi Radio",
    "TV Box",
    "Computer",
    "Player",
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
