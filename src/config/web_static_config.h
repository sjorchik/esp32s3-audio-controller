#pragma once

// Константи віддачі вбудованих веб-ресурсів (Prompt 19): net/web_static.cpp.

namespace web_static_cfg {

// Cache-Control для сторінок. no-cache = браузер не використовує копію без запиту,
// тож після оновлення прошивки (у т.ч. OTA) користувач одразу бачить нові сторінки.
constexpr const char* kCacheControl = "no-cache";

// Усі вбудовані ресурси стиснуті gzip (tools/build_web.py).
constexpr const char* kContentEncoding = "gzip";

static_assert(kCacheControl[0] != '\0', "Cache-Control must not be empty");
static_assert(kContentEncoding[0] != '\0', "Content-Encoding must not be empty");

}  // namespace web_static_cfg
