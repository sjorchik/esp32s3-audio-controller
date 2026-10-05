#pragma once

// Таблиця вбудованих веб-ресурсів (Prompt 19). Рукописний заголовок; визначення
// kWebAssets/kWebAssetCount генерує tools/build_web.py у net/web_assets_gen.cpp
// (webui/** -> gzip -> const uint8_t[] у flash).
// Кожен запис - ОДИН URL-шлях; псевдоніми (/ -> /index.html, /x -> /x.html) - окремі
// записи, що вказують на ті самі байти.

#include <stddef.h>
#include <stdint.h>

struct WebAsset {
    const char* path;     // абсолютний URL-шлях: "/", "/index.html", "/css/app.css"
    const char* mime;     // повний Content-Type (з charset для тексту)
    const uint8_t* data;  // gzip-байти (у flash, .rodata)
    size_t len;           // довжина data
};

extern const WebAsset kWebAssets[];
extern const size_t kWebAssetCount;
