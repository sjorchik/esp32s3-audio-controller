#pragma once

// Значення за замовчуванням без «магічних» чисел у модулях.

#include <stdint.h>

namespace defaults {

// --- Консоль ---
constexpr uint32_t kSerialBaud = 115200;

// --- I2C ---
// Рівні: підтяжки до 3.3 В, швидкість 100 кГц.
// Процесор живиться від 9 В, пороги логічної одиниці звірити за даташитом.
constexpr uint32_t kI2cClockHz = 100000;
constexpr uint8_t kAudioProcessorI2cAddr = 0x44;

// --- Аудіопроцесор ---
// Діапазони уточнюються під конкретний чип у його драйвері.
constexpr int8_t kDefaultVolume  = 40;
constexpr int8_t kDefaultBass    = 0;
constexpr int8_t kDefaultTreble  = 0;
constexpr int8_t kDefaultBalance = 0;
constexpr bool   kDefaultMute    = false;
constexpr bool   kDefaultLoudness = false;

// [Prompt 46b] Логічні входи: 0 = WiFi Radio (цифровий шлях, селектор НЕ задіяний), 1..4 = зовнішні
// джерела. Зовнішній логічний k відповідає входу чіпа k - 1 (audio_cfg::chipInputFor()).
// TDA7318: радіо + 4 зовнішні (усі 5 логічних); PT2313L: радіо + 3 зовнішні (логічний 4 недоступний).
// Назви редагуються у вебі; тут — типові. Вхід 4 за замовчуванням вимкнений (settings_cfg::kInputMaskDefault).
constexpr uint8_t kDefaultInput = 0;
constexpr uint8_t kInputCount = 5;
constexpr const char* kInputNames[kInputCount] = {
    "WiFi Radio",
    "Bluetooth",
    "TV Box",
    "Computer",
    "AUX",
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