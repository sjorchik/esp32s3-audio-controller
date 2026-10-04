#pragma once

// Константи модуля net/web_server (Prompt 13; [Prompt 14] станції; [Prompt 15] IR;
// [Prompt 16] OTA; watchdog-фікс).
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
// [Prompt 15] IR (/api/ir*)
// ---------------------------------------------------------------------------
// Максимальний розмір тіла POST /api/ir/map/import (байти): масив
// [{"action":"VOL_UP","addr":0,"cmd":16,"rc5x":false},...] ~ 55-60 байт на запис.
// Більше за kMaxBodyBytes (1024), тож власний ліміт; більше -> 413. Має вміщати
// найбільшу мапу, яку приймає IrRc5::importJson() (перевір за ємністю мапи в
// config/ir_config.h при зміні).
constexpr size_t kIrImportMaxBytes = 8192;

// Максимум символів імені дії в DELETE /api/ir/map/{action} (найдовше: BALANCE_DOWN).
constexpr size_t kIrActionNameMaxChars = 24;

// ---------------------------------------------------------------------------
// [Prompt 16] OTA (POST /api/ota)
// ---------------------------------------------------------------------------
// Перший байт образу ESP32.
constexpr uint8_t kOtaImageMagic = 0xE9;

// Менше за це — явно не прошивка -> 400 bad_image. Верхньої межі ТУТ немає навмисно:
// вона береться з розміру вільного OTA-розділу під час роботи (partitions.csv), щоб
// не дублювати цифру й не розійтись із таблицею розділів.
constexpr size_t kOtaMinImageBytes = 4096;

// Пауза між відправкою відповіді й ESP.restart(). Має з запасом перекривати час, за який
// lwip віддасть відповідь клієнту.
constexpr uint32_t kOtaRestartDelayMs = 1500;

// Одноразова задача перезапуску.
constexpr int      kOtaRestartTaskCore     = 0;
constexpr uint8_t  kOtaRestartTaskPriority = 1;
constexpr uint32_t kOtaRestartStackBytes   = 3072;

// Віддача процесора після кожного шматка тіла (тіки FreeRTOS). Запис у флеш у задачі
// async_tcp безперервно навантажує ядро; без паузи IDLE0 не встигає годувати сторожовий
// таймер. Ціна: ~1 мс на шматок (~2 с на 2,5 МБ при 1000 Гц тіку).
constexpr uint32_t kOtaChunkYieldTicks = 1;

// Таймаут сторожового таймера задач (TWDT) на час OTA та після неї. IDLE0 лишається
// підписаним (відписування дає лавину "esp_task_wdt_reset: task not found" з idle-хука),
// ми лише подовжуємо таймаут: флеш-операції на ядрі 0 можуть тривати довше за типові 5 с.
// kOtaWdtNormalTimeoutMs ПОВИННО дорівнювати CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 у збірці
// (Arduino-ESP32 за замовчуванням 5 с); це значення повертається на невдачі OTA.
constexpr uint32_t kOtaWdtTimeoutMs       = 60000;
constexpr uint32_t kOtaWdtNormalTimeoutMs = 5000;

// Крок логу прогресу в Serial (відсотки); 0 = не логувати.
constexpr uint8_t kOtaLogStepPercent = 10;

// Буфери тексту помилки у відповіді (error — код, reason — Update.errorString() тощо).
constexpr size_t kOtaErrorBufBytes  = 24;
constexpr size_t kOtaReasonBufBytes = 64;

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
static_assert(kIrImportMaxBytes >= 4096, "IR map import limit too small");
static_assert(kIrActionNameMaxChars >= 12, "IR action name limit too small (BALANCE_DOWN)");
static_assert(kStationIndexMaxDigits >= 3 && kStationIndexMaxDigits <= 5,
              "station index digits out of range");
static_assert(kOtaMinImageBytes >= 1024, "OTA min image size too small");
static_assert(kOtaRestartDelayMs >= 500, "restart delay too short to flush the response");
static_assert(kOtaWdtTimeoutMs >= kOtaWdtNormalTimeoutMs && kOtaWdtNormalTimeoutMs >= 1000,
              "OTA watchdog timeout must not be shorter than the normal one");
static_assert(kOtaChunkYieldTicks >= 1, "OTA chunk yield must be at least 1 tick");
static_assert(kOtaLogStepPercent <= 100, "OTA log step is a percentage");
static_assert(kOtaRestartTaskCore == 0, "web runs on core 0 (MASTER SPEC, section 5)");
static_assert(kOtaRestartTaskPriority >= 1 && kOtaRestartTaskPriority <= display_cfg::kTaskPriority,
              "network/web priority must be the lowest (<= UI)");
static_assert(kOtaErrorBufBytes >= 16 && kOtaReasonBufBytes >= 32, "OTA message buffers too small");

}  // namespace web_cfg
