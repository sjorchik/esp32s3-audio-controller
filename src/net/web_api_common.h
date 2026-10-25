#pragma once

// Спільні хелпери нових маршрутів (Prompt 18): net/web_api_player.cpp, web_api_system.cpp.
// Формат відповідей навмисно ТОЙ САМИЙ, що в web_server.cpp (P13-P16):
//   помилка: {"ok":false,"error":"<код>"[,"field":"<поле>"]} + HTTP-код;
//   успіх:   {"ok":true,...}.
// Усе inline (header-only): жодного нового .cpp, жодних зв'язків із анонімним
// простором імен web_server.cpp.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <stdlib.h>
#include <string.h>

#include "config/web_api_config.h"
#include "config/web_server_config.h"  // web_cfg::kMaxBodyBytes, WEB_SERVER_DEBUG
#include "core/app_state.h"

#if WEB_SERVER_DEBUG
#define WEB_API_LOG(fmt, ...) Serial.printf("[WEB] " fmt "\n", ##__VA_ARGS__)
#else
#define WEB_API_LOG(...) ((void)0)
#endif

namespace web_api {

inline const char* modeName(Mode m) {
    switch (m) {
        case Mode::Standby:       return "Standby";
        case Mode::Radio:         return "Radio";
        case Mode::ExternalInput: return "ExternalInput";
        case Mode::Menu:          return "Menu";
        case Mode::IrLearn:       return "IrLearn";
        case Mode::WifiSetup:     return "WifiSetup";
        case Mode::OtaUpdate:     return "OtaUpdate";
    }
    return "Unknown";
}

inline void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    AsyncResponseStream* r = req->beginResponseStream("application/json");
    if (r == nullptr) {
        req->send(503);
        return;
    }
    r->setCode(code);
    r->addHeader("Cache-Control", "no-store");
    serializeJson(doc, *r);
    req->send(r);
}

inline void sendError(AsyncWebServerRequest* req, int code, const char* error) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = error;
    sendJson(req, code, doc);
}

inline void sendFieldError(AsyncWebServerRequest* req, int code, const char* error,
                           const char* field) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = error;
    if (field != nullptr) doc["field"] = field;
    sendJson(req, code, doc);
}

// {"ok":false,"error":"out_of_range","field":"value","min":0,"max":100} + 400
inline void sendRangeError(AsyncWebServerRequest* req, const char* field, int lo, int hi) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "out_of_range";
    doc["field"] = field;
    doc["min"] = lo;
    doc["max"] = hi;
    sendJson(req, 400, doc);
}

// ESPAsyncWebServer зіставляє "/x" і з "/x/...": обробники точного шляху перевіряють url самі.
inline bool exactUrl(AsyncWebServerRequest* req, const char* path) {
    if (req->url() == path) return true;
    sendError(req, 404, "not_found");
    return false;
}

// Збирає мале тіло в malloc-буфер, який AsyncWebServerRequest сам звільняє
// (_tempObject). Те саме, що handleJsonBody() у web_server.cpp.
inline void jsonBodyCallback(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
                             size_t total) {
    if (total == 0 || total > web_cfg::kMaxBodyBytes) return;
    if (index == 0) {
        req->_tempObject = malloc(total + 1);
    }
    char* buf = static_cast<char*>(req->_tempObject);
    if (buf == nullptr || index + len > total) return;
    memcpy(buf + index, data, len);
    if (index + len == total) buf[total] = '\0';
}

// Читає й розбирає JSON-обʼєкт із тіла. При помилці сам шле відповідь і повертає false.
inline bool readJsonObjectBody(AsyncWebServerRequest* req, JsonDocument& in) {
    if (req->contentLength() > web_cfg::kMaxBodyBytes) {
        sendError(req, 413, "body_too_large");
        return false;
    }
    const char* body = static_cast<const char*>(req->_tempObject);
    if (req->contentLength() == 0 || body == nullptr) {
        sendError(req, 400, "no_body");
        return false;
    }
    const DeserializationError de = deserializeJson(in, body, req->contentLength());
    if (de || !in.is<JsonObject>()) {
        sendError(req, 400, "invalid_json");
        return false;
    }
    return true;
}

// Дозволені ключі кореня (1..2). Невідомий ключ -> 400 unknown_field (друкарська помилка
// клієнта не має виглядати як успіх).
inline bool allowKeys(AsyncWebServerRequest* req, JsonObjectConst root, const char* a,
                      const char* b = nullptr) {
    for (JsonPairConst kv : root) {
        const char* k = kv.key().c_str();
        if (strcmp(k, a) != 0 && (b == nullptr || strcmp(k, b) != 0)) {
            sendFieldError(req, 400, "unknown_field", "unknown");
            return false;
        }
    }
    return true;
}

// Dev-CORS (config/web_api_config.h: WEB_API_DEV_CORS). При 0 — порожня функція.
// При 1: заголовки Access-Control-Allow-* додаються до КОЖНОЇ відповіді (DefaultHeaders
// бібліотеки, тож це чіпає й обробники P13-P16, не змінюючи їх), а на OPTIONS /api/* —
// порожня відповідь 204 для preflight-запитів браузера.
inline void registerDevCors(AsyncWebServer& server) {
#if WEB_API_DEV_CORS
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin",
                                         web_api_cfg::kCorsAllowOrigin);
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods",
                                         web_api_cfg::kCorsAllowMethods);
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers",
                                         web_api_cfg::kCorsAllowHeaders);
    server.on("/api/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) { req->send(204); });
    Serial.println("[WEB] WARNING: WEB_API_DEV_CORS enabled (development only)");
#else
    (void)server;
#endif
}

}  // namespace web_api
