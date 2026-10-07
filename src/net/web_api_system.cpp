// net/web_api_system.cpp (Prompt 18): GET /api/system, POST /api/system/reboot,
// POST /api/system/factory-reset, POST /api/wifi/reset.
//
// Перезапуск виконує одноразова задача ПІСЛЯ відповіді (HTTP-обробник нічого не блокує).
// Небезпечні дії (factory-reset, wifi/reset) вимагають {"confirm":true} у тілі, щоб
// випадковий запит (наприклад, помилковий клік чи скрипт) нічого не стер.
//
// Звідки береться «скидання Wi-Fi»: WifiManager не має публічного методу скидання (лише
// forceReset при старті, утримання OK). [Prompt 29] Тут стираються ВСІ збережені мережі:
// WifiNetworks::clear() (порожній список у NVS) і облікові дані esp_wifi — та сама
// послідовність, що в wifi_manager.cpp::resetCredentials() — WiFi.disconnect(false, true), але
// в одноразовій задачі безпосередньо перед ESP.restart(), коли WifiManager уже все одно буде
// перезапущено. Після старту збережених мереж немає — WifiManager піднімає AP "AudioCtrl-Setup".
//
// [Prompt 29] registerSystemRoutes() заодно реєструє /api/wifi* (net/web_api_wifi.cpp), тож
// web_server.cpp не змінювався.

#include "net/web_api_system.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdint.h>
#include <string.h>

#include "core/app_state.h"
#include "core/settings.h"
#include "net/web_api_common.h"
#include "net/web_api_wifi.h"
#include "net/wifi_manager.h"
#include "net/wifi_networks.h"

namespace {

using namespace web_api;

// ---------------------------------------------------------------------------
// Імена
// ---------------------------------------------------------------------------
const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "PowerOn";
        case ESP_RST_EXT:       return "External";
        case ESP_RST_SW:        return "Software";
        case ESP_RST_PANIC:     return "Panic";
        case ESP_RST_INT_WDT:   return "IntWatchdog";
        case ESP_RST_TASK_WDT:  return "TaskWatchdog";
        case ESP_RST_WDT:       return "OtherWatchdog";
        case ESP_RST_DEEPSLEEP: return "DeepSleep";
        case ESP_RST_BROWNOUT:  return "Brownout";
        case ESP_RST_SDIO:      return "Sdio";
        default:                return "Other";  // числовий код — у resetReasonCode
    }
}

const char* wifiStateName(WifiState s) {
    switch (s) {
        case WifiState::Connecting:        return "Connecting";
        case WifiState::Connected:         return "Connected";
        case WifiState::ApMode:            return "ApMode";
        case WifiState::ApClientConnected: return "ApClientConnected";
        case WifiState::Off:               return "Off";  // [Prompt 29]
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// GET /api/system
// ---------------------------------------------------------------------------
void handleSystem(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/system")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    const AppStateData st = AppState::snapshot();
    const esp_reset_reason_t rr = esp_reset_reason();

    char ssid[33];
    char ip[16];
    WifiManager::copyInfo(ssid, sizeof(ssid), ip, sizeof(ip));

    JsonDocument doc;

    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["version"] = web_api_cfg::kFirmwareVersion;
    fw["buildDate"] = __DATE__;  // "Oct  4 2026" (формат компілятора)
    fw["buildTime"] = __TIME__;  // "17:42:11"
    fw["sdk"] = ESP.getSdkVersion();

    JsonObject chip = doc["chip"].to<JsonObject>();
    chip["model"] = ESP.getChipModel();
    chip["revision"] = ESP.getChipRevision();
    chip["cores"] = ESP.getChipCores();
    chip["cpuMhz"] = ESP.getCpuFreqMHz();

    doc["mode"] = modeName(st.mode);
    doc["uptimeMs"] = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    doc["resetReason"] = resetReasonName(rr);
    doc["resetReasonCode"] = static_cast<int>(rr);

    JsonObject heap = doc["heap"].to<JsonObject>();
    heap["total"] = ESP.getHeapSize();
    heap["free"] = ESP.getFreeHeap();
    heap["min"] = ESP.getMinFreeHeap();
    heap["largestBlock"] = ESP.getMaxAllocHeap();

    JsonObject psram = doc["psram"].to<JsonObject>();
    psram["total"] = ESP.getPsramSize();
    psram["free"] = ESP.getFreePsram();

    doc["flash"]["sizeBytes"] = ESP.getFlashChipSize();

    // LittleFS: usedBytes() обходить блоки файлової системи — може тривати десятки мс.
    JsonObject fsInfo = doc["fs"].to<JsonObject>();
    fsInfo["totalBytes"] = static_cast<uint32_t>(LittleFS.totalBytes());
    fsInfo["usedBytes"] = static_cast<uint32_t>(LittleFS.usedBytes());

    // OTA: maxImageBytes — розмір вільного OTA-розділу (верхня межа для POST /api/ota).
    JsonObject ota = doc["ota"].to<JsonObject>();
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    ota["runningPartition"] = (run != nullptr) ? run->label : "";
    ota["nextPartition"] = (next != nullptr) ? next->label : "";
    ota["maxImageBytes"] = (next != nullptr) ? static_cast<uint32_t>(next->size) : 0u;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["state"] = wifiStateName(WifiManager::state());
    wifi["mode"] = WifiManager::isApMode() ? "AP" : "STA";
    wifi["ssid"] = ssid;
    wifi["ip"] = ip;
    wifi["rssi"] = WifiManager::rssi();

    JsonObject settings = doc["settings"].to<JsonObject>();
    settings["dirty"] = SettingsStore::isDirty();
    settings["writeCount"] = SettingsStore::writeCount();

    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// Відкладений перезапуск
// ---------------------------------------------------------------------------
enum class RestartKind : uint8_t { Reboot, FactoryReset, WifiReset };

volatile bool s_restartPending = false;  // чіпається лише з задачі async_tcp

void restartTask(void* arg) {
    const RestartKind kind = static_cast<RestartKind>(reinterpret_cast<uintptr_t>(arg));
    vTaskDelay(pdMS_TO_TICKS(web_api_cfg::kRestartDelayMs));  // відповідь має піти клієнту

    switch (kind) {
        case RestartKind::Reboot:
            SettingsStore::flush();  // відкладені налаштування не повинні загубитись
            break;
        case RestartKind::FactoryReset:
            // Свідомо БЕЗ flush: resetToDefaults() = дефолти в кеш + негайний запис ключа
            // налаштувань (мапа IR, станції, Wi-Fi не чіпаються).
            SettingsStore::resetToDefaults();
            Serial.println("[WEB] factory reset: settings restored to defaults");
            break;
        case RestartKind::WifiReset:
            SettingsStore::flush();
            // [Prompt 29] усі збережені мережі: порожній список у NVS (НЕ видалення ключа —
            // інакше наступний старт знову імпортував би стару мережу з esp_wifi)
            if (WifiNetworks::clear() != WifiNetResult::Ok) {
                Serial.println("[WEB] wifi reset: WARNING network list not erased");
            }
            WiFi.disconnect(false, true);  // стерти «останню мережу» esp_wifi (як resetCredentials())
            vTaskDelay(pdMS_TO_TICKS(web_api_cfg::kWifiEraseSettleMs));
            Serial.println("[WEB] wifi reset: saved networks erased");
            break;
    }
    WifiNetworks::persistIfDirty();  // [Prompt 29] відкладений запис списку не має загубитись
    Serial.println("[WEB] restarting");
    Serial.flush();
    ESP.restart();
    vTaskDelete(nullptr);
}

// Перевіряє, чи можна перезапускати, створює задачу й шле відповідь.
void scheduleRestart(AsyncWebServerRequest* req, RestartKind kind, const char* next) {
    if (AppState::snapshot().mode == Mode::OtaUpdate) {
        sendError(req, 409, "ota_in_progress");
        return;
    }
    if (s_restartPending) {
        sendError(req, 409, "restart_pending");
        return;
    }
    s_restartPending = true;
    const BaseType_t ok = xTaskCreatePinnedToCore(
        restartTask, "api_restart", web_api_cfg::kRestartStackBytes,
        reinterpret_cast<void*>(static_cast<uintptr_t>(kind)), web_api_cfg::kRestartTaskPriority,
        nullptr, web_api_cfg::kRestartTaskCore);
    if (ok != pdPASS) {
        s_restartPending = false;
        sendError(req, 500, "internal");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["restarting"] = true;
    doc["inMs"] = web_api_cfg::kRestartDelayMs;
    if (next != nullptr) doc["next"] = next;

    AsyncResponseStream* r = req->beginResponseStream("application/json");
    if (r == nullptr) {
        req->send(503);  // задача перезапуску вже створена: пристрій усе одно перезапуститься
        return;
    }
    r->addHeader("Cache-Control", "no-store");
    r->addHeader("Connection", "close");
    serializeJson(doc, *r);
    req->send(r);
}

// Тіло {"confirm":true} обовʼязкове. При відмові сама шле відповідь і повертає false.
bool requireConfirm(AsyncWebServerRequest* req) {
    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return false;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "confirm")) return false;
    JsonVariantConst v = root["confirm"];
    if (!v.is<bool>() || !v.as<bool>()) {
        sendFieldError(req, 400, "confirm_required", "confirm");
        return false;
    }
    return true;
}

void handleReboot(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/system/reboot")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    scheduleRestart(req, RestartKind::Reboot, nullptr);
}

void handleFactoryReset(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/system/factory-reset")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!requireConfirm(req)) return;
    scheduleRestart(req, RestartKind::FactoryReset, nullptr);
}

void handleWifiReset(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/wifi/reset")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!requireConfirm(req)) return;
    scheduleRestart(req, RestartKind::WifiReset, "ap_setup");
}

}  // namespace

void registerSystemRoutes(AsyncWebServer& server) {
    // Методи різні, але "/api/system" (GET) зіставилось би й з "/api/system/..." — обробник
    // перевіряє url сам; точні POST-підшляхи реєструємо першими.
    server.on("/api/system/reboot", HTTP_POST, handleReboot);
    server.on("/api/system/factory-reset", HTTP_POST, handleFactoryReset, nullptr,
              web_api::jsonBodyCallback);
    server.on("/api/system", HTTP_GET, handleSystem);
    server.on("/api/wifi/reset", HTTP_POST, handleWifiReset, nullptr, web_api::jsonBodyCallback);

    // [Prompt 29] Скан, збережені мережі, примусове підключення. Після "/api/wifi/reset":
    // "/api/wifi" (GET) зіставилось би й з "/api/wifi/...", обробники перевіряють url самі.
    registerWifiRoutes(server);
}
