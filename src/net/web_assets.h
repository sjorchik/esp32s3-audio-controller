#pragma once

// Таблиця вбудованих веб-ресурсів (Prompt 19). Рукописний заголовок; визначення
// kWebAssets/kWebAssetCount генерує tools/build_web.py у net/web_assets_gen.cpp
// (webui/** -> gzip -> const uint8_t[] у flash).
// Кожен запис - ОДИН URL-шлях; псевдоніми (/ -> /index.html, /x -> /x.html) - окремі
// записи, що вказують на ті самі байти.
// [W1] ДОДАНО поле etag (в КІНЕЦЬ структури).

#include <stddef.h>
#include <stdint.h>

struct WebAsset {
    const char* path;     // абсолютний URL-шлях: "/", "/index.html", "/css/app.css"
    const char* mime;     // повний Content-Type (з charset для тексту)
    const uint8_t* data;  // байти (у flash, .rodata): gzip, або сирі, якщо gzip не зменшує розмір
    size_t len;           // довжина data
    bool gzip;            // (H2) true -> віддавати з Content-Encoding: gzip; false -> сирі байти (PNG)
    const char* etag;     // (W1) значення ETag РАЗОМ з лапками, напр. "\"0123abcd0123abcd\""
};

extern const WebAsset kWebAssets[];
extern const size_t kWebAssetCount;
