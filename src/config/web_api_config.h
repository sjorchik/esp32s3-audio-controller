#pragma once

// Константи модулів net/web_api_* (Prompt 18): команди керування, система, dev-CORS.
// Константи Prompt 13-16 лишаються в config/web_server_config.h (web_cfg::); тут їх
// не дублюємо. Часові значення — у мілісекундах.

#include <stddef.h>
#include <stdint.h>

#include "config/display_config.h"  // display_cfg::kTaskPriority (для static_assert)

// ---------------------------------------------------------------------------
// Dev-режим CORS. 0 = вимкнено (за замовчуванням, для продакшену).
// 1 = до КОЖНОЇ відповіді (включно зі старими /api/*) додаються заголовки
// Access-Control-Allow-*, а на OPTIONS /api/* відповідь 204. Дозволяє розробляти
// сторінки на ПК (відкриваючи їх з file:// чи локального сервера), звертаючись до
// пристрою за адресою audio.local, без перепрошивки LittleFS. НЕ вмикати в релізі.
// ---------------------------------------------------------------------------
#define WEB_API_DEV_CORS 0

namespace web_api_cfg {

// ---------------------------------------------------------------------------
// Інформація про прошивку (GET /api/system)
// ---------------------------------------------------------------------------
// У репозиторії немає файлу з версією, тому вона живе тут: єдине місце, яке треба
// міняти вручну перед релізом. Дата/час збірки беруться з __DATE__/__TIME__ файлу
// net/web_api_system.cpp (оновлюються лише коли перекомпільовано САМЕ цей файл).
constexpr const char* kFirmwareVersion = "0.18.0";

// ---------------------------------------------------------------------------
// POST /api/volume {"step":N}: максимальний модуль кроку (одиниці шкали гучності 0..100).
// ---------------------------------------------------------------------------
constexpr int kVolumeStepAbsMax = 100;

// ---------------------------------------------------------------------------
// Відкладений перезапуск (reboot / factory-reset / wifi-reset)
// ---------------------------------------------------------------------------
// Пауза між відправкою відповіді й ESP.restart(); має з запасом перекривати час, за який
// lwip віддасть відповідь клієнту (те саме міркування, що web_cfg::kOtaRestartDelayMs).
constexpr uint32_t kRestartDelayMs = 1500;

// Невелика пауза після стирання облікових даних Wi-Fi перед перезапуском.
constexpr uint32_t kWifiEraseSettleMs = 200;

// Одноразова задача перезапуску. Стек 4096: усередині NVS-запис (resetToDefaults/flush)
// і WiFi.disconnect(false, true).
constexpr int      kRestartTaskCore     = 0;
constexpr uint8_t  kRestartTaskPriority = 1;
constexpr uint32_t kRestartStackBytes   = 4096;

// ---------------------------------------------------------------------------
// Dev-CORS (діє лише при WEB_API_DEV_CORS 1)
// ---------------------------------------------------------------------------
constexpr const char* kCorsAllowOrigin  = "*";
constexpr const char* kCorsAllowMethods = "GET, POST, PUT, DELETE, OPTIONS";
constexpr const char* kCorsAllowHeaders = "Content-Type";

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kFirmwareVersion[0] != '\0', "firmware version must not be empty");
static_assert(kVolumeStepAbsMax >= 1 && kVolumeStepAbsMax <= 127, "volume step limit out of range");
static_assert(kRestartDelayMs >= 500, "restart delay too short to flush the response");
static_assert(kWifiEraseSettleMs <= kRestartDelayMs, "wifi erase settle must be short");
static_assert(kRestartTaskCore == 0, "web runs on core 0 (MASTER SPEC, section 5)");
static_assert(kRestartTaskPriority >= 1 && kRestartTaskPriority <= display_cfg::kTaskPriority,
              "network/web priority must be the lowest (<= UI)");
static_assert(kRestartStackBytes >= 3072, "restart task stack too small");

}  // namespace web_api_cfg
