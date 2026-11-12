// net/web_api_usb.cpp [USB15]: веб-API плеєра USB. Контракт — docs/web_api.md, §16; огляд маршрутів — net/web_api_usb.h.
//
// Принципи:
//  - Усі зміни стану режиму «USB» (вхід / вихід, shuffle / repeat) йдуть через AppController::runWebCommand() (WebCmdType::
//    UsbModeSet / UsbOptionsSet): та сама відкладена ціль навігації й той самий шлях NVS, що в RC5. Команди черги / каталогу
//    (cd, up, play, next, prev, stop), пауза / продовження і seek викликають ті самі неблокуючі функції usb_queue / usb_browser /
//    AudioPlayer, що й кнопки пульта в AppController (usbPlayerKey): вони призначені для виклику з будь-якої задачі (USB10).
//  - Усе, що вимагає читання флешки, виконує задача usb_nav; відповідь на команду — {"ok":true,"reqId":N} (HTTP 202), результат
//    — GET /api/usb/result?id=N або lastReq у GET /api/usb/status. Перевірки, які можна зробити за знімком списку (застарілий
//    список, індекс поза межами, не той тип запису), виконуються ОДРАЗУ й дають 409 / 400 без постановки команди; команда все
//    одно ставиться з expectVersion, тож гонка зі зміною списку закривається в usb_nav.
//  - Формат помилок і хелпери — net/web_api_common.h (той самий, що P13–P18).

#include "net/web_api_usb.h"

#include "config/features.h"

#if FEATURE_USB_MODE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_player.h"
#include "config/usb_browser_config.h"
#include "config/web_api_usb_config.h"
#include "core/app_controller.h"
#include "core/app_state.h"
#include "net/web_api_common.h"
#include "usb/usb_browser.h"
#include "usb/usb_mode.h"
#include "usb/usb_queue.h"

namespace {

using namespace web_api;

// ---------------------------------------------------------------------------
// Відповіді
// ---------------------------------------------------------------------------
// {"ok":false,"error":"<код>","<key>":"<val>"} + HTTP-код (key / val — статичні рядки).
void sendUsbError(AsyncWebServerRequest* req, int code, const char* error, const char* key = nullptr,
                  const char* val = nullptr) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = error;
    if (key != nullptr && val != nullptr) {
        doc[key] = val;
    }
    sendJson(req, code, doc);
}

// Команду прийнято в чергу usb_nav (виконується асинхронно) -> 202 {"ok":true,"reqId":N}.
void sendAccepted(AsyncWebServerRequest* req, uint32_t reqId) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["reqId"] = reqId;
    sendJson(req, 202, doc);
}

// Відмова постановки команди (usb_browser / usb_queue request*): why — статичний рядок. Код помилки = why.
void sendRefused(AsyncWebServerRequest* req, const char* why) {
    if (why == nullptr) {
        why = "refused";
    }
    if (strcmp(why, "mode_not_ready") == 0) {
        sendUsbError(req, 409, "usb_mode_off", "state", usb_mode::stateName(usb_mode::state()));
        return;
    }
    const int code = (strcmp(why, "task_create_failed") == 0) ? 503 : 409;
    sendError(req, code, why);
}

void sendRunResult(AsyncWebServerRequest* req, WebCmdResult r, const char* why) {
    switch (r) {
        case WebCmdResult::Busy:
            sendError(req, 503, "busy");
            return;
        case WebCmdResult::NotAllowed:
            sendError(req, 409, why != nullptr ? why : "not_allowed");
            return;
        case WebCmdResult::OutOfRange:
            sendError(req, 400, why != nullptr ? why : "out_of_range");
            return;
        case WebCmdResult::Ok:
            return;  // обробляє викликач
    }
}

// ---------------------------------------------------------------------------
// Перевірки стану (409). Повертають true, якщо ПРОДОВЖУВАТИ (відповідь не надіслано).
// ---------------------------------------------------------------------------
// Усі POST-маршрути: під час OTA / навчання IR — за правилами §1.2 контракту.
bool passDeviceGate(AsyncWebServerRequest* req) {
    const Mode m = AppState::snapshot().mode;
    if (m == Mode::OtaUpdate) {
        sendError(req, 409, "ota_in_progress");
        return false;
    }
    if (m == Mode::IrLearn) {
        sendError(req, 409, "ir_learn_active");
        return false;
    }
    return true;
}

bool passModeGate(AsyncWebServerRequest* req) {
    const usb_mode::State s = usb_mode::state();
    if (s == usb_mode::State::Ready || s == usb_mode::State::Playing) {
        return true;
    }
    sendUsbError(req, 409, "usb_mode_off", "state", usb_mode::stateName(s));
    return false;
}

bool passMediaGate(AsyncWebServerRequest* req) {
    const usb_mode::Media m = usb_mode::media();
    if (m == usb_mode::Media::Mounted) {
        return true;
    }
    sendUsbError(req, 409, "no_media", "media", usb_mode::mediaName(m));
    return false;
}

// ---------------------------------------------------------------------------
// Розбір тіла / параметрів
// ---------------------------------------------------------------------------
// Дозволені ключі кореня (до 3): інакше 400 unknown_field (web_api::allowKeys підтримує лише 2).
bool allowKeys3(AsyncWebServerRequest* req, JsonObjectConst root, const char* a, const char* b, const char* c) {
    for (JsonPairConst kv : root) {
        const char* k = kv.key().c_str();
        if (strcmp(k, a) != 0 && strcmp(k, b) != 0 && strcmp(k, c) != 0) {
            sendFieldError(req, 400, "unknown_field", "unknown");
            return false;
        }
    }
    return true;
}

// Невідʼємне ціле з рядка запиту (лише цифри, ≤ 10 знаків, ≤ UINT32_MAX). present = параметр є в запиті.
// false -> уже надіслано 400 invalid_value.
bool queryUint(AsyncWebServerRequest* req, const char* name, uint32_t* out, bool* present) {
    *present = false;
    const AsyncWebParameter* p = req->getParam(name);
    if (p == nullptr) {
        return true;
    }
    *present = true;
    const String& v = p->value();
    if (v.length() == 0 || v.length() > 10) {
        sendFieldError(req, 400, "invalid_value", name);
        return false;
    }
    uint64_t n = 0;
    for (size_t i = 0; i < v.length(); ++i) {
        const char c = v[i];
        if (c < '0' || c > '9') {
            sendFieldError(req, 400, "invalid_value", name);
            return false;
        }
        n = n * 10u + static_cast<uint32_t>(c - '0');
    }
    if (n > 0xFFFFFFFFull) {
        sendFieldError(req, 400, "invalid_value", name);
        return false;
    }
    *out = static_cast<uint32_t>(n);
    return true;
}

// ---------------------------------------------------------------------------
// JSON-екранування імен (UTF-8). Імена з флешки можуть містити невалідний UTF-8: непарні байти замінюються на U+FFFD.
// ---------------------------------------------------------------------------
// Довжина коректної UTF-8 послідовності з p (2..4) або 0, якщо послідовність невалідна. Рядок завершується '\0'.
size_t utf8SeqLen(const uint8_t* p) {
    auto cont = [](uint8_t b) { return (b & 0xC0) == 0x80; };
    const uint8_t c = p[0];
    if (c >= 0xC2 && c <= 0xDF) {
        return cont(p[1]) ? 2 : 0;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(p[1]) || !cont(p[2])) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;  // надмірне кодування
        if (c == 0xED && p[1] > 0x9F) return 0;  // сурогати
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(p[1]) || !cont(p[2]) || !cont(p[3])) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;
        if (c == 0xF4 && p[1] > 0x8F) return 0;
        return 4;
    }
    return 0;
}

// Розкладає рядок на шматки JSON-рядка (без лапок) і віддає їх у emit(const char*, size_t). Один код і для підрахунку довжини,
// і для запису: розміри в бюджеті сторінки гарантовано збігаються з реально надісланими.
template <typename Emit>
void jsonEscapePieces(const char* s, Emit emit) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
    while (*p != 0) {
        const uint8_t c = *p;
        if (c == '"') {
            emit("\\\"", 2);
            ++p;
        } else if (c == '\\') {
            emit("\\\\", 2);
            ++p;
        } else if (c < 0x20) {
            char b[8];
            const int n = snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c));
            emit(b, static_cast<size_t>(n));
            ++p;
        } else if (c < 0x80) {
            const uint8_t* q = p + 1;  // серія звичайних ASCII-символів одним шматком
            while (*q >= 0x20 && *q < 0x80 && *q != '"' && *q != '\\') {
                ++q;
            }
            emit(reinterpret_cast<const char*>(p), static_cast<size_t>(q - p));
            p = q;
        } else {
            const size_t n = utf8SeqLen(p);
            if (n != 0) {
                emit(reinterpret_cast<const char*>(p), n);
                p += n;
            } else {
                emit("\\ufffd", 6);
                ++p;
            }
        }
    }
}

size_t jsonEscapedLen(const char* s) {
    size_t total = 0;
    jsonEscapePieces(s, [&total](const char*, size_t n) { total += n; });
    return total;
}

void jsonPrintString(Print& out, const char* s) {
    out.write('"');
    jsonEscapePieces(s, [&out](const char* piece, size_t n) { out.write(reinterpret_cast<const uint8_t*>(piece), n); });
    out.write('"');
}

const char* baseName(const char* path) {
    const char* sl = strrchr(path, '/');
    return (sl != nullptr) ? sl + 1 : path;
}

// [USB18a] Папка треку: усе до останнього '/' ("/" для файла в корені).
String parentDir(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash == nullptr || slash == path) {
        return String("/");
    }
    return String(path).substring(0, static_cast<unsigned int>(slash - path));
}

// ---------------------------------------------------------------------------
// GET /api/usb/status
// ---------------------------------------------------------------------------
void handleStatus(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/status")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    const usb_mode::State ms = usb_mode::state();
    const usb_mode::Info inf = usb_mode::info();
    usb_browser::Status bs;
    usb_queue::Status qs;
    if (!usb_browser::status(&bs) || !usb_queue::status(&qs)) {
        sendError(req, 503, "busy");
        return;
    }
    char cwd[usb_browser_cfg::kPathMax];
    const bool haveCwd = usb_browser::cwd(cwd, sizeof(cwd));
    char cur[usb_browser_cfg::kPathMax];
    const bool haveCur = usb_queue::currentPath(cur, sizeof(cur));

    JsonDocument doc;
    doc["active"] = (ms == usb_mode::State::Ready || ms == usb_mode::State::Playing);

    JsonObject mode = doc["mode"].to<JsonObject>();
    mode["state"] = usb_mode::stateName(ms);
    mode["reason"] = inf.reason;  // nullptr -> null
    doc["media"] = usb_mode::mediaName(usb_mode::media());

    if (haveCwd) {
        doc["cwd"] = String(cwd);
    } else {
        doc["cwd"] = nullptr;
    }

    JsonObject list = doc["list"].to<JsonObject>();
    list["valid"] = bs.valid;
    list["version"] = bs.version;
    list["count"] = bs.count;
    list["dirs"] = bs.dirs;
    list["depth"] = bs.depth;
    list["truncated"] = bs.truncated;

    JsonObject q = doc["queue"].to<JsonObject>();
    q["state"] = usb_queue::stateName(qs.state);
    q["index"] = qs.index;
    q["count"] = qs.count;
    q["position"] = qs.position;
    q["pos"] = qs.pos;  // [USB18a] 1-based, 0 = немає
    q["shuffle"] = qs.shuffle;
    q["repeat"] = qs.repeatAll;
    q["paused"] = qs.paused;
    q["prepared"] = qs.prepared;
    q["reason"] = qs.reason;  // nullptr -> null

    // «Зараз грає»: трек черги, що стартує або грає (на паузі — теж). Позиція / тривалість — кеш плеєра, 1 с.
    if (haveCur && (qs.state == usb_queue::State::Playing || qs.state == usb_queue::State::Starting)) {
        JsonObject np = doc["nowPlaying"].to<JsonObject>();
        np["name"] = String(baseName(cur));
        np["path"] = String(cur);    // [USB18a] повний шлях від кореня носія (як у list / play)
        np["dir"] = parentDir(cur);  // [USB18a] папка черги
        np["posSec"] = AudioPlayer::filePositionSec();
        np["durSec"] = AudioPlayer::fileDurationSec();
        np["paused"] = qs.paused;
        // [USB18a] ID3: лише теги САМЕ цього файлу (хеш шляху); відсутні / порожні — без поля.
        AudioPlayer::FileTags tg;
        if (AudioPlayer::fileTags(cur, &tg)) {
            if (tg.title[0] != '\0') np["title"] = String(tg.title);
            if (tg.artist[0] != '\0') np["artist"] = String(tg.artist);
            if (tg.album[0] != '\0') np["album"] = String(tg.album);
        }
    } else {
        doc["nowPlaying"] = nullptr;
    }

    if (bs.lastReqId != 0) {
        JsonObject lr = doc["lastReq"].to<JsonObject>();
        lr["id"] = bs.lastReqId;
        lr["ok"] = bs.lastOk;
        lr["why"] = bs.lastWhy;  // nullptr -> null
    } else {
        doc["lastReq"] = nullptr;
    }
    doc["pending"] = bs.pending;

    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// GET /api/usb/list?offset=&limit=[&version=]
// ---------------------------------------------------------------------------
void handleList(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/list")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    uint32_t offset = 0;
    uint32_t limit = web_api_usb_cfg::kListDefaultLimit;
    uint32_t version = 0;
    bool present = false;
    if (!queryUint(req, "offset", &offset, &present)) return;
    if (!queryUint(req, "limit", &limit, &present)) return;
    if (!queryUint(req, "version", &version, &present)) return;
    if (limit < 1 || limit > web_api_usb_cfg::kListMaxLimit) {
        sendRangeError(req, "limit", 1, web_api_usb_cfg::kListMaxLimit);
        return;
    }
    if (offset > 65535u) {
        sendRangeError(req, "offset", 0, 65535);
        return;
    }
    if (!passModeGate(req) || !passMediaGate(req)) return;

    char* buf = static_cast<char*>(heap_caps_malloc(web_api_usb_cfg::kListRawBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        sendError(req, 503, "busy");
        return;
    }
    char cwd[usb_browser_cfg::kPathMax];
    usb_browser::PageInfo pi;
    const bool got = usb_browser::readPage(static_cast<uint16_t>(offset), static_cast<uint16_t>(limit), version, buf,
                                           web_api_usb_cfg::kListRawBufBytes, cwd, sizeof(cwd), &pi);
    if (!got) {
        free(buf);
        sendError(req, 503, "busy");
        return;
    }
    if (!pi.valid) {
        free(buf);
        sendError(req, 409, "list_invalid");
        return;
    }
    if (pi.stale) {
        free(buf);
        sendUsbError(req, 409, "stale");
        return;
    }
    if (offset > pi.count) {
        free(buf);
        sendRangeError(req, "offset", 0, pi.count);
        return;
    }

    // Скільки записів вміщається в бюджет JSON (перший — завжди; імена НЕ обрізаються).
    uint16_t shown = 0;
    {
        size_t used = 0;
        const char* p = buf;
        for (uint16_t k = 0; k < pi.returned; ++k) {
            const char* name = p + 1;
            const size_t len = strlen(name);
            const size_t cost = web_api_usb_cfg::kListItemOverheadBytes + jsonEscapedLen(name);
            if (shown > 0 && used + cost > web_api_usb_cfg::kListJsonBudgetBytes) {
                break;
            }
            used += cost;
            ++shown;
            p = name + len + 1;
        }
    }

    AsyncResponseStream* r = req->beginResponseStream("application/json");
    if (r == nullptr) {
        free(buf);
        req->send(503);
        return;
    }
    r->setCode(200);
    r->addHeader("Cache-Control", "no-store");
    r->printf("{\"version\":%lu,\"cwd\":", static_cast<unsigned long>(pi.version));
    jsonPrintString(*r, cwd);
    r->printf(",\"depth\":%u,\"count\":%u,\"dirs\":%u,\"truncated\":%s,\"offset\":%u,\"limit\":%u,\"items\":[",
              static_cast<unsigned>(pi.depth), static_cast<unsigned>(pi.count), static_cast<unsigned>(pi.dirs),
              pi.truncated ? "true" : "false", static_cast<unsigned>(offset), static_cast<unsigned>(limit));
    {
        const char* p = buf;
        for (uint16_t k = 0; k < shown; ++k) {
            const bool isDir = (p[0] != 0);
            const char* name = p + 1;
            if (k > 0) {
                r->write(',');
            }
            r->printf("{\"i\":%u,\"name\":", static_cast<unsigned>(offset + k));
            jsonPrintString(*r, name);
            r->printf(",\"dir\":%s}", isDir ? "true" : "false");
            p = name + strlen(name) + 1;
        }
    }
    r->print("],\"nextOffset\":");
    if (offset + shown < pi.count) {
        r->printf("%u", static_cast<unsigned>(offset + shown));
    } else {
        r->print("null");
    }
    r->write('}');
    free(buf);
    req->send(r);
}

// ---------------------------------------------------------------------------
// GET /api/usb/result?id=N
// ---------------------------------------------------------------------------
void handleResult(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/result")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    uint32_t id = 0;
    bool present = false;
    if (!queryUint(req, "id", &id, &present)) return;
    if (!present) {
        sendFieldError(req, 400, "missing_field", "id");
        return;
    }
    usb_browser::ReqResult rr;
    if (!usb_browser::result(id, &rr)) {
        sendError(req, 503, "busy");
        return;
    }
    if (rr.state == usb_browser::ReqResult::State::Unknown) {
        sendError(req, 404, "unknown_req");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    JsonObject o = doc["req"].to<JsonObject>();
    o["id"] = id;
    if (rr.state == usb_browser::ReqResult::State::Pending) {
        o["done"] = false;
    } else {
        o["done"] = true;
        o["ok"] = rr.ok;
        o["why"] = rr.why;  // nullptr -> null
    }
    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// POST /api/usb/mode {"action":"enter"|"exit"|"toggle"}
// ---------------------------------------------------------------------------
void handleMode(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/mode")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "action")) return;
    JsonVariantConst v = root["action"];
    if (v.isNull()) {
        sendFieldError(req, 400, "missing_field", "action");
        return;
    }
    if (!v.is<const char*>()) {
        sendFieldError(req, 400, "invalid_value", "action");
        return;
    }
    const char* a = v.as<const char*>();
    int32_t code = -1;
    if (strcmp(a, "exit") == 0) code = 0;
    else if (strcmp(a, "enter") == 0) code = 1;
    else if (strcmp(a, "toggle") == 0) code = 2;
    if (code < 0) {
        sendFieldError(req, 400, "invalid_value", "action");
        return;
    }

    const usb_mode::State before = usb_mode::state();
    const bool inBefore = (before == usb_mode::State::Entering || before == usb_mode::State::Ready ||
                           before == usb_mode::State::Playing);
    const char* why = nullptr;
    const WebCmdResult r = AppController::runWebCommand({WebCmdType::UsbModeSet, code, false}, &why);
    if (r != WebCmdResult::Ok) {
        sendRunResult(req, r, why);
        return;
    }
    const bool wantEnter = (code == 1) || (code == 2 && !inBefore);
    const bool changed = wantEnter ? !inBefore : (before != usb_mode::State::Off);

    JsonDocument doc;
    doc["ok"] = true;
    doc["changed"] = changed;
    doc["state"] = usb_mode::stateName(usb_mode::state());
    sendJson(req, changed ? 202 : 200, doc);
}

// ---------------------------------------------------------------------------
// POST /api/usb/cd {"index":N[,"version":V]} | {"up":true}
// POST /api/usb/play {"index":N[,"version":V]}
// ---------------------------------------------------------------------------
void handleCdOrPlay(AsyncWebServerRequest* req, bool isPlay) {
    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (isPlay) {
        if (!allowKeys(req, root, "index", "version")) return;
    } else {
        if (!allowKeys3(req, root, "index", "version", "up")) return;
    }

    const bool hasIndex = !root["index"].isNull();
    const bool hasVersion = !root["version"].isNull();
    const bool hasUp = !isPlay && !root["up"].isNull();

    if (hasUp) {
        if (!root["up"].is<bool>() || !root["up"].as<bool>()) {
            sendFieldError(req, 400, "invalid_value", "up");
            return;
        }
        if (hasIndex || hasVersion) {
            sendFieldError(req, 400, "conflicting_fields", hasIndex ? "index" : "version");
            return;
        }
    } else if (!hasIndex) {
        sendFieldError(req, 400, "missing_field", "index");
        return;
    }
    int32_t index = 0;
    uint32_t version = 0;
    if (hasIndex) {
        JsonVariantConst iv = root["index"];
        if (!iv.is<int>()) {
            sendFieldError(req, 400, "invalid_value", "index");
            return;
        }
        index = iv.as<int>();
        if (index < 0 || index > 65534) {
            sendRangeError(req, "index", 0, 65534);
            return;
        }
    }
    if (hasVersion) {
        JsonVariantConst vv = root["version"];
        if (!vv.is<uint32_t>()) {
            sendFieldError(req, 400, "invalid_value", "version");
            return;
        }
        version = vv.as<uint32_t>();
    }

    if (!passDeviceGate(req) || !passModeGate(req) || !passMediaGate(req)) return;

    usb_browser::Status st;
    if (!usb_browser::status(&st)) {
        sendError(req, 503, "busy");
        return;
    }
    if (!st.valid) {
        sendError(req, 409, "list_invalid");
        return;
    }

    uint32_t reqId = 0;
    const char* why = nullptr;
    if (hasUp) {
        if (st.depth == 0) {
            sendError(req, 409, "at_root");
            return;
        }
        if (!usb_browser::requestUp(&reqId, &why)) {
            sendRefused(req, why);
            return;
        }
        sendAccepted(req, reqId);
        return;
    }

    if (version != 0 && version != st.version) {
        sendError(req, 409, "stale");
        return;
    }
    if (index >= static_cast<int32_t>(st.count)) {
        sendRangeError(req, "index", 0, st.count > 0 ? static_cast<int>(st.count) - 1 : 0);
        return;
    }
    const bool isDir = index < static_cast<int32_t>(st.dirs);  // папки першими (usb_browser.h)
    if (isPlay && isDir) {
        sendFieldError(req, 400, "not_file", "index");
        return;
    }
    if (!isPlay && !isDir) {
        sendFieldError(req, 400, "not_dir", "index");
        return;
    }
    // expectVersion = версія, за якою щойно перевірено індекс: якщо список змінився між перевіркою й виконанням — «stale».
    const uint32_t expect = (version != 0) ? version : st.version;
    const bool ok = isPlay ? usb_queue::requestPlayIndex(static_cast<uint16_t>(index), expect, &reqId, &why)
                           : usb_browser::requestEnterDir(static_cast<uint16_t>(index), expect, &reqId, &why);
    if (!ok) {
        sendRefused(req, why);
        return;
    }
    sendAccepted(req, reqId);
}

void handleCd(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/cd")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    handleCdOrPlay(req, false);
}

void handlePlay(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/play")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    handleCdOrPlay(req, true);
}

// ---------------------------------------------------------------------------
// POST /api/usb/control {"action":"pause"|"resume"|"toggle"|"next"|"prev"|"stop"}
// ---------------------------------------------------------------------------
void sendPlayResult(AsyncWebServerRequest* req, const char* result, bool changed) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["result"] = result;
    doc["changed"] = changed;
    sendJson(req, 200, doc);
}

void handleControl(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/control")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "action")) return;
    JsonVariantConst v = root["action"];
    if (v.isNull()) {
        sendFieldError(req, 400, "missing_field", "action");
        return;
    }
    if (!v.is<const char*>()) {
        sendFieldError(req, 400, "invalid_value", "action");
        return;
    }
    const char* a = v.as<const char*>();
    enum class Act : uint8_t { Pause, Resume, Toggle, Next, Prev, Stop, Bad };
    Act act = Act::Bad;
    if (strcmp(a, "pause") == 0) act = Act::Pause;
    else if (strcmp(a, "resume") == 0) act = Act::Resume;
    else if (strcmp(a, "toggle") == 0) act = Act::Toggle;
    else if (strcmp(a, "next") == 0) act = Act::Next;
    else if (strcmp(a, "prev") == 0) act = Act::Prev;
    else if (strcmp(a, "stop") == 0) act = Act::Stop;
    if (act == Act::Bad) {
        sendFieldError(req, 400, "invalid_value", "action");
        return;
    }

    if (!passDeviceGate(req) || !passModeGate(req)) return;

    uint32_t reqId = 0;
    const char* why = nullptr;
    if (act == Act::Next || act == Act::Prev || act == Act::Stop) {
        const bool ok = (act == Act::Next)   ? usb_queue::requestNext(&reqId, &why)
                        : (act == Act::Prev) ? usb_queue::requestPrev(&reqId, &why)
                                             : usb_queue::requestStop(&reqId, &why);
        if (!ok) {
            sendRefused(req, why);
            return;
        }
        sendAccepted(req, reqId);
        return;
    }

    usb_queue::Status qs;
    if (!usb_queue::status(&qs)) {
        sendError(req, 503, "busy");
        return;
    }
    // requestPlayPause() = той самий перемикач, що OK на пульті: Playing -> пауза / продовження, Idle + prepared -> запуск.
    if (act == Act::Pause) {
        if (qs.state != usb_queue::State::Playing) {
            sendError(req, 409, "not_playing");
            return;
        }
        if (qs.paused) {
            sendPlayResult(req, "paused", false);  // ідемпотентно
            return;
        }
    } else if (act == Act::Resume) {
        if (qs.state == usb_queue::State::Playing && !qs.paused) {
            sendPlayResult(req, "playing", false);  // ідемпотентно
            return;
        }
    }
    const usb_queue::PlayPause pp = usb_queue::requestPlayPause(&why);
    switch (pp) {
        case usb_queue::PlayPause::Paused:
            sendPlayResult(req, "paused", true);
            return;
        case usb_queue::PlayPause::Resumed:
            sendPlayResult(req, "resumed", true);
            return;
        case usb_queue::PlayPause::Started:
            sendPlayResult(req, "started", true);
            return;
        case usb_queue::PlayPause::Refused:
            sendRefused(req, why);
            return;
    }
    sendError(req, 500, "internal");
}

// ---------------------------------------------------------------------------
// POST /api/usb/seek {"deltaSec":±N}
// ---------------------------------------------------------------------------
void handleSeek(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/seek")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "deltaSec")) return;
    JsonVariantConst v = root["deltaSec"];
    if (v.isNull()) {
        sendFieldError(req, 400, "missing_field", "deltaSec");
        return;
    }
    if (!v.is<int>()) {
        sendFieldError(req, 400, "invalid_value", "deltaSec");
        return;
    }
    const int32_t delta = v.as<int>();
    if (delta < -web_api_usb_cfg::kSeekMaxAbsSec || delta > web_api_usb_cfg::kSeekMaxAbsSec) {
        sendRangeError(req, "deltaSec", -web_api_usb_cfg::kSeekMaxAbsSec, web_api_usb_cfg::kSeekMaxAbsSec);
        return;
    }

    if (!passDeviceGate(req) || !passModeGate(req)) return;
    if (!AudioPlayer::fileActive()) {
        sendError(req, 409, "not_playing");
        return;
    }
    if (AudioPlayer::filePaused()) {
        sendError(req, 409, "paused");
        return;
    }
    bool atEnd = false;  // [USB18a] ціль за кінцем файла = кінець треку (див. AudioPlayer::seekFileSec)
    if (delta != 0 && !AudioPlayer::seekFileSec(delta, &atEnd)) {
        sendError(req, 409, "seek_refused");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["deltaSec"] = delta;
    doc["atEnd"] = atEnd;  // [USB18a]
    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// POST /api/usb/options {"shuffle":bool,"repeat":bool}
// ---------------------------------------------------------------------------
void handleOptions(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/usb/options")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "shuffle", "repeat")) return;

    int32_t mask = 0;
    JsonVariantConst sv = root["shuffle"];
    JsonVariantConst rv = root["repeat"];
    if (!sv.isNull()) {
        if (!sv.is<bool>()) {
            sendFieldError(req, 400, "invalid_value", "shuffle");
            return;
        }
        mask |= 0x1 | (sv.as<bool>() ? 0x2 : 0);
    }
    if (!rv.isNull()) {
        if (!rv.is<bool>()) {
            sendFieldError(req, 400, "invalid_value", "repeat");
            return;
        }
        mask |= 0x4 | (rv.as<bool>() ? 0x8 : 0);
    }
    if (mask == 0) {
        sendFieldError(req, 400, "missing_field", "shuffle");
        return;
    }

    if (!passDeviceGate(req) || !passModeGate(req)) return;
    const char* why = nullptr;
    const WebCmdResult r = AppController::runWebCommand({WebCmdType::UsbOptionsSet, mask, false}, &why);
    if (r != WebCmdResult::Ok) {
        sendRunResult(req, r, why);
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["shuffle"] = usb_queue::shuffle();
    doc["repeat"] = usb_queue::repeatAll();
    sendJson(req, 200, doc);
}

}  // namespace

void registerUsbRoutes(AsyncWebServer& server) {
    // Точні шляхи; обробники самі перевіряють url (exactUrl), як решта web_api_*.
    server.on("/api/usb/status", HTTP_GET, handleStatus);
    server.on("/api/usb/list", HTTP_GET, handleList);
    server.on("/api/usb/result", HTTP_GET, handleResult);
    server.on("/api/usb/mode", HTTP_POST, handleMode, nullptr, web_api::jsonBodyCallback);
    server.on("/api/usb/cd", HTTP_POST, handleCd, nullptr, web_api::jsonBodyCallback);
    server.on("/api/usb/play", HTTP_POST, handlePlay, nullptr, web_api::jsonBodyCallback);
    server.on("/api/usb/control", HTTP_POST, handleControl, nullptr, web_api::jsonBodyCallback);
    server.on("/api/usb/seek", HTTP_POST, handleSeek, nullptr, web_api::jsonBodyCallback);
    server.on("/api/usb/options", HTTP_POST, handleOptions, nullptr, web_api::jsonBodyCallback);
}

#else  // !FEATURE_USB_MODE

void registerUsbRoutes(AsyncWebServer& server) { (void)server; }

#endif  // FEATURE_USB_MODE
