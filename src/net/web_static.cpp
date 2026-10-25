// net/web_static.cpp (Prompt 19, H2, W1): GET-маршрути для вбудованих ресурсів (gzip або сирі байти).
// [W1] ETag + 304 Not Modified на If-None-Match (Cache-Control лишається no-cache: браузер
// завжди питає, але тіло повторно не качає); під WEB_TRACE підключає діагностику запитів.

#include "net/web_static.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <string.h>

#include "config/web_server_config.h"  // WEB_SERVER_DEBUG, WEB_TRACE
#include "config/web_static_config.h"
#include "net/web_assets.h"
#include "net/web_trace.h"

namespace {

// If-None-Match може містити список і слабкі теги (W/"..."): шукаємо наш тег як підрядок.
bool etagMatches(AsyncWebServerRequest* req, const char* etag) {
    if (etag == nullptr || etag[0] == '\0') {
        return false;
    }
    const AsyncWebHeader* h = req->getHeader("If-None-Match");
    if (h == nullptr) {
        return false;
    }
    return strstr(h->value().c_str(), etag) != nullptr;
}

void serveAsset(AsyncWebServerRequest* req, const WebAsset* asset) {
    // ESPAsyncWebServer зіставляє "/x" і з "/x/...": віддаємо лише точний збіг.
    if (req->url() != asset->path) {
        req->send(404, "text/plain", "Not found");
        return;
    }
    // (W1) Клієнт уже має цю версію -> 304 без тіла.
    if (etagMatches(req, asset->etag)) {
        AsyncWebServerResponse* nm = req->beginResponse(304);
        if (nm == nullptr) {
            req->send(503);
            return;
        }
        nm->addHeader("ETag", asset->etag);
        nm->addHeader("Cache-Control", web_static_cfg::kCacheControl);
        req->send(nm);
        return;
    }
    // Дані читаються безпосередньо з flash (AsyncProgmemResponse), копії в RAM немає.
    AsyncWebServerResponse* res = req->beginResponse(200, asset->mime, asset->data, asset->len);
    if (res == nullptr) {
        req->send(503);
        return;
    }
    // (H2) Двійкові ресурси, які gzip не зменшує (PNG), лежать сирими - без Content-Encoding.
    if (asset->gzip) {
        res->addHeader("Content-Encoding", web_static_cfg::kContentEncoding);
    }
    res->addHeader("Cache-Control", web_static_cfg::kCacheControl);
    if (asset->etag != nullptr && asset->etag[0] != '\0') {
        res->addHeader("ETag", asset->etag);
    }
    req->send(res);
}

}  // namespace

void registerStaticRoutes(AsyncWebServer& server) {
    for (size_t i = 0; i < kWebAssetCount; ++i) {
        const WebAsset* asset = &kWebAssets[i];
        server.on(asset->path, HTTP_GET,
                  [asset](AsyncWebServerRequest* req) { serveAsset(req, asset); });
    }
#if WEB_SERVER_DEBUG
    Serial.printf("[WEB] static: %u routes registered\n", static_cast<unsigned>(kWebAssetCount));
#endif
    web_trace::begin(server);  // [W1] no-op при WEB_TRACE = 0
}
