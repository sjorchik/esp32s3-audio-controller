// net/web_static.cpp (Prompt 19): GET-маршрути для вбудованих gzip-ресурсів.

#include "net/web_static.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "config/web_server_config.h"  // WEB_SERVER_DEBUG
#include "config/web_static_config.h"
#include "net/web_assets.h"

namespace {

void serveAsset(AsyncWebServerRequest* req, const WebAsset* asset) {
    // ESPAsyncWebServer зіставляє "/x" і з "/x/...": віддаємо лише точний збіг.
    if (req->url() != asset->path) {
        req->send(404, "text/plain", "Not found");
        return;
    }
    // Дані читаються безпосередньо з flash (AsyncProgmemResponse), копії в RAM немає.
    AsyncWebServerResponse* res = req->beginResponse(200, asset->mime, asset->data, asset->len);
    if (res == nullptr) {
        req->send(503);
        return;
    }
    res->addHeader("Content-Encoding", web_static_cfg::kContentEncoding);
    res->addHeader("Cache-Control", web_static_cfg::kCacheControl);
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
}
