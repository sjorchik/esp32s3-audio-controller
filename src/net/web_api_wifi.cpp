// net/web_api_wifi.cpp (Prompt 29): див. web_api_wifi.h. Формат відповідей і помилок —
// той самий, що в решти /api/* (web_api_common.h).

#include "net/web_api_wifi.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <string.h>

#include "config/wifi_config.h"
#include "core/app_state.h"
#include "net/web_api_common.h"
#include "net/wifi_manager.h"
#include "net/wifi_networks.h"

namespace {

using namespace web_api;

constexpr const char* kNetworksPrefix = "/api/wifi/networks/";

const char* wifiStateName(WifiState s) {
    switch (s) {
        case WifiState::Connecting:        return "Connecting";
        case WifiState::Connected:         return "Connected";
        case WifiState::ApMode:            return "ApMode";
        case WifiState::ApClientConnected: return "ApClientConnected";
        case WifiState::Off:               return "Off";
    }
    return "Unknown";
}

// Гейт для скану й змін мережі. При відмові сам шле відповідь.
bool gate(AsyncWebServerRequest* req) {
    const AppStateData st = AppState::snapshot();
    if (st.mode == Mode::OtaUpdate) {
        sendError(req, 409, "ota_in_progress");
        return false;
    }
    if (st.mode == Mode::IrLearn) {
        sendError(req, 409, "ir_learn_active");
        return false;
    }
    if (st.restarting) {
        sendError(req, 503, "busy");
        return false;
    }
    return true;
}

// Відповідь на помилку списку. false, якщо res == Ok.
bool sendNetError(AsyncWebServerRequest* req, WifiNetResult res, const char* indexField) {
    switch (res) {
        case WifiNetResult::Ok:               return false;
        case WifiNetResult::Busy:             sendError(req, 503, "busy"); break;
        case WifiNetResult::InvalidSsid:      sendFieldError(req, 400, "wifi_ssid_invalid", "ssid"); break;
        case WifiNetResult::InvalidPassword:  sendFieldError(req, 400, "wifi_password_invalid", "password"); break;
        case WifiNetResult::ListFull:         sendError(req, 409, "wifi_list_full"); break;
        case WifiNetResult::IndexOutOfRange:  sendFieldError(req, 400, "index_out_of_range", indexField); break;
        case WifiNetResult::PositionOutOfRange: sendFieldError(req, 400, "index_out_of_range", "position"); break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// GET /api/wifi
// ---------------------------------------------------------------------------
void handleWifiGet(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/wifi")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    char ssid[33];
    char ip[16];
    WifiManager::copyInfo(ssid, sizeof(ssid), ip, sizeof(ip));
    WifiNetInfo nets[wifi_cfg::kMaxSavedNetworks];
    const int8_t n = WifiNetworks::list(nets, wifi_cfg::kMaxSavedNetworks);
    if (n < 0) {
        sendError(req, 503, "busy");
        return;
    }
    const bool connected = WifiManager::isConnected();

    JsonDocument doc;
    doc["connected"] = connected;
    doc["state"] = wifiStateName(WifiManager::state());
    doc["ssid"] = ssid;
    doc["ip"] = ip;
    doc["rssi"] = WifiManager::rssi();
    doc["channel"] = WifiManager::channel();
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int8_t i = 0; i < n; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["index"] = i;
        o["ssid"] = nets[i].ssid;
        o["secure"] = nets[i].secure;
        o["current"] = connected && strcmp(nets[i].ssid, ssid) == 0;
    }
    doc["max"] = wifi_cfg::kMaxSavedNetworks;
    doc["persisted"] = !WifiNetworks::isDirty();
    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// Скан
// ---------------------------------------------------------------------------
void handleScanStart(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/wifi/scan")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!gate(req)) return;
    switch (WifiManager::requestScan()) {
        case WifiRequest::Accepted: {
            JsonDocument doc;
            doc["ok"] = true;
            doc["state"] = "running";
            sendJson(req, 200, doc);
            break;
        }
        case WifiRequest::ScanBusy: sendError(req, 409, "wifi_scan_busy"); break;
        case WifiRequest::LockBusy: sendError(req, 503, "busy"); break;
        default:                    sendError(req, 409, "wifi_busy"); break;
    }
}

void handleScanGet(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/wifi/scan")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    uint32_t age = 0;
    const WifiScanState st = WifiManager::scanState(&age);
    JsonDocument doc;
    switch (st) {
        case WifiScanState::Idle:    doc["state"] = "idle"; break;
        case WifiScanState::Running: doc["state"] = "running"; break;
        case WifiScanState::Failed:  doc["state"] = "failed"; break;
        case WifiScanState::Done:    doc["state"] = "done"; break;
    }
    JsonArray arr = doc["networks"].to<JsonArray>();
    if (st == WifiScanState::Done) {
        WifiScanEntry res[wifi_cfg::kMaxNetworks];
        const uint8_t n = WifiManager::copyScan(res, wifi_cfg::kMaxNetworks);
        doc["ageMs"] = age;
        for (uint8_t i = 0; i < n; ++i) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"] = res[i].ssid;
            o["rssi"] = res[i].rssi;
            o["channel"] = res[i].channel;
            o["secure"] = res[i].secure;
            o["saved"] = WifiNetworks::find(res[i].ssid) >= 0;
        }
    }
    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// POST /api/wifi/networks {ssid, password?, position?}
// ---------------------------------------------------------------------------
void handleNetworksAdd(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/wifi/networks")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!gate(req)) return;

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    for (JsonPairConst kv : root) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "ssid") != 0 && strcmp(k, "password") != 0 && strcmp(k, "position") != 0) {
            sendFieldError(req, 400, "unknown_field", "unknown");
            return;
        }
    }
    JsonVariantConst vs = root["ssid"];
    if (!vs.is<const char*>()) {
        sendFieldError(req, 400, "wifi_ssid_invalid", "ssid");
        return;
    }
    const char* ssid = vs.as<const char*>();

    const char* password = nullptr;  // немає поля: нова — відкрита, наявна — без змін
    if (!root["password"].isNull()) {
        JsonVariantConst vp = root["password"];
        if (!vp.is<const char*>()) {
            sendFieldError(req, 400, "wifi_password_invalid", "password");
            return;
        }
        password = vp.as<const char*>();
    }
    int position = -1;
    if (!root["position"].isNull()) {
        JsonVariantConst vpos = root["position"];
        if (!vpos.is<int>() || vpos.as<int>() < 0) {
            sendFieldError(req, 400, "invalid_value", "position");
            return;
        }
        position = vpos.as<int>();
    }

    uint8_t idx = 0;
    bool updated = false;
    const WifiNetResult r = WifiNetworks::add(ssid, password, position, false, &idx, &updated);
    if (sendNetError(req, r, "index")) return;

    // Лог без пароля.
    WEB_API_LOG("wifi network %s: \"%s\" -> #%u", updated ? "updated" : "added", ssid,
                static_cast<unsigned>(idx));
    JsonDocument doc;
    doc["ok"] = true;
    doc["index"] = idx;
    doc["updated"] = updated;
    sendJson(req, updated ? 200 : 201, doc);
}

// POST /api/wifi/networks/move {from, to}
void handleNetworksMove(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/wifi/networks/move")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!gate(req)) return;

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "from", "to")) return;
    JsonVariantConst vf = root["from"];
    JsonVariantConst vt = root["to"];
    if (!vf.is<int>()) {
        sendFieldError(req, 400, "invalid_value", "from");
        return;
    }
    if (!vt.is<int>()) {
        sendFieldError(req, 400, "invalid_value", "to");
        return;
    }
    const int from = vf.as<int>();
    const int to = vt.as<int>();
    const int n = WifiNetworks::count();
    if (from < 0 || from >= n) {
        sendFieldError(req, 400, "index_out_of_range", "from");
        return;
    }
    if (to < 0 || to >= n) {
        sendFieldError(req, 400, "index_out_of_range", "to");
        return;
    }
    const WifiNetResult r =
        WifiNetworks::move(static_cast<uint8_t>(from), static_cast<uint8_t>(to));
    if (sendNetError(req, r, "from")) return;
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
}

// DELETE /api/wifi/networks/{index}
void handleNetworksDelete(AsyncWebServerRequest* req) {
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!gate(req)) return;

    const String url = req->url();
    if (!url.startsWith(kNetworksPrefix)) {
        sendError(req, 404, "not_found");
        return;
    }
    const char* p = url.c_str() + strlen(kNetworksPrefix);
    const size_t len = strlen(p);
    if (len == 0 || len > 3) {
        sendError(req, 400, "invalid_index");
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit(static_cast<unsigned char>(p[i]))) {
            sendError(req, 400, "invalid_index");
            return;
        }
    }
    const unsigned long idx = strtoul(p, nullptr, 10);
    if (idx >= wifi_cfg::kMaxSavedNetworks) {
        sendError(req, 404, "not_found");
        return;
    }
    const WifiNetResult r = WifiNetworks::remove(static_cast<uint8_t>(idx));
    if (r == WifiNetResult::IndexOutOfRange) {
        sendError(req, 404, "not_found");
        return;
    }
    if (sendNetError(req, r, "index")) return;
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
}

// POST /api/wifi/connect {index}
void handleConnect(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/wifi/connect")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!gate(req)) return;

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "index")) return;
    JsonVariantConst vi = root["index"];
    if (!vi.is<int>()) {
        sendFieldError(req, 400, "invalid_value", "index");
        return;
    }
    const int idx = vi.as<int>();
    if (idx < 0 || idx >= wifi_cfg::kMaxSavedNetworks) {
        sendFieldError(req, 400, "index_out_of_range", "index");
        return;
    }
    switch (WifiManager::requestConnect(static_cast<uint8_t>(idx))) {
        case WifiRequest::Accepted: {
            JsonDocument doc;
            doc["ok"] = true;
            doc["switching"] = true;
            doc["inMs"] = wifi_cfg::kConnectReplyFlushMs;
            AsyncResponseStream* r = req->beginResponseStream("application/json");
            if (r == nullptr) {
                req->send(503);  // запит уже прийнято: пристрій усе одно перемкнеться
                return;
            }
            r->addHeader("Cache-Control", "no-store");
            r->addHeader("Connection", "close");
            serializeJson(doc, *r);
            req->send(r);
            break;
        }
        case WifiRequest::AlreadyCurrent: {
            JsonDocument doc;
            doc["ok"] = true;
            doc["switching"] = false;
            sendJson(req, 200, doc);
            break;
        }
        case WifiRequest::NoSuchNetwork: sendFieldError(req, 400, "index_out_of_range", "index"); break;
        case WifiRequest::LockBusy:      sendError(req, 503, "busy"); break;
        default:                         sendError(req, 409, "wifi_busy"); break;
    }
}

}  // namespace

void registerWifiRoutes(AsyncWebServer& server) {
    // ПОРЯДОК ВАЖЛИВИЙ (ESPAsyncWebServer зіставляє "/x" і з "/x/..."): спершу точніші шляхи.
    server.on("/api/wifi/networks/move", HTTP_POST, handleNetworksMove, nullptr,
              web_api::jsonBodyCallback);
    server.on("/api/wifi/networks/*", HTTP_DELETE, handleNetworksDelete);
    server.on("/api/wifi/networks", HTTP_POST, handleNetworksAdd, nullptr,
              web_api::jsonBodyCallback);
    server.on("/api/wifi/connect", HTTP_POST, handleConnect, nullptr, web_api::jsonBodyCallback);
    server.on("/api/wifi/scan", HTTP_POST, handleScanStart);
    server.on("/api/wifi/scan", HTTP_GET, handleScanGet);
    server.on("/api/wifi", HTTP_GET, handleWifiGet);
}
