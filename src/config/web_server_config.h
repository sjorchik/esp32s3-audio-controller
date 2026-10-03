#pragma once

// Константи модуля net/web_server (Prompt 13).
// Порт НЕ дублюється: береться з wifi_cfg::kMdnsHttpPort (той самий, що оголошує
// mDNS), щоб оголошений і фактичний порт не розійшлись.
// Часові значення — у мілісекундах.

#include <stddef.h>
#include <stdint.h>

#include "config/defaults.h"
#include "config/display_config.h"  // display_cfg::kTaskPriority (для static_assert)
#include "config/station_store_config.h"  // [Prompt 14] kMaxFileBytes/kNameMax/kUrlMax
#include "config/wifi_config.h"

// Serial-лог запитів: "[WEB] GET /api/status". Вимкнути = 0.
#define WEB_SERVER_DEBUG 1

namespace web_cfg {

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
constexpr uint16_t kHttpPort = wifi_cfg::kMdnsHttpPort;

// Максимальний розмір тіла POST /api/settings (байти). Найбільше легітимне тіло —
// inputNames (4 x 31 байт + службові символи) ~ 250 байт; з запасом.
// ArduinoJson 7 виділяє памʼять динамічно, тож «розміру буфера документа» немає:
// обмежуємо саме вхідне тіло.
constexpr size_t kMaxBodyBytes = 1024;

// ---------------------------------------------------------------------------
// [Prompt 14] Станції (/api/stations*)
// ---------------------------------------------------------------------------
// Максимальний розмір тіла POST /api/stations/import (байти) = ліміт файлу, який
// приймає StationStore::importXxx(); більше -> 413.
constexpr size_t kStationsImportMaxBytes = station_store_cfg::kMaxFileBytes;

// Тимчасовий файл, куди складається завантажений вміст перед імпортом
// (видаляється після кожного імпорту). Один імпорт одночасно.
constexpr const char* kStationsImportTmpPath = "/st_web_import.tmp";

// Файл, який exportJson() пише для GET /api/stations/export (перезаписується).
constexpr const char* kStationsExportPath = "/st_web_export.json";

// Імʼя файлу в Content-Disposition.
constexpr const char* kStationsExportFilename = "stations.json";

// Максимум цифр в {index} у /api/stations/{index}.
constexpr size_t kStationIndexMaxDigits = 4;

// ---------------------------------------------------------------------------
// Одноразова задача-стартер (чекає на Connected, піднімає сервер, видаляє себе)
// ---------------------------------------------------------------------------
constexpr int      kStarterTaskCore     = 0;
constexpr uint8_t  kStarterTaskPriority = 1;
constexpr uint32_t kStarterStackBytes   = 4096;
constexpr uint32_t kStartPollMs         = 500;

// ---------------------------------------------------------------------------
// Межі значень POST /api/settings
// (тембр/баланс — з AudioProcessorCapabilities, тут їх немає навмисно)
// ---------------------------------------------------------------------------
constexpr int kBrightnessMin = 0;
constexpr int kBrightnessMax = 100;

// Назва входу: 1..31 байт UTF-8 (буфер Settings::inputNames[i] = 32 з '\0';
// рівність перевіряє static_assert у web_server.cpp).
constexpr size_t kInputNameMaxBytes = 31;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kStarterTaskCore == 0, "web runs on core 0 (MASTER SPEC, section 5)");
static_assert(kStarterTaskPriority >= 1 && kStarterTaskPriority <= display_cfg::kTaskPriority,
              "network/web priority must be the lowest (<= UI)");
static_assert(kStartPollMs >= 1, "poll period must be >= 1 ms");
static_assert(kMaxBodyBytes >= 256, "POST body limit too small for inputNames");
static_assert(kBrightnessMin >= 0 && kBrightnessMax <= 100 && kBrightnessMin <= kBrightnessMax,
              "brightness is 0..100 %");
static_assert(kInputNameMaxBytes >= 1, "input name must hold at least 1 byte");
static_assert(kStationsImportMaxBytes >= 4096, "station import limit too small");
static_assert(kStationsImportTmpPath[0] == '/' && kStationsExportPath[0] == '/',
              "station temp/export paths must be absolute LittleFS paths");
static_assert(kStationIndexMaxDigits >= 3 && kStationIndexMaxDigits <= 5,
              "station index digits out of range");

}  // namespace web_cfg
