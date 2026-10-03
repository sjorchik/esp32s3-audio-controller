// net/web_server.cpp (Prompt 13): інфраструктура, /api/status, /api/settings.
// [Prompt 14] ДОДАНО: /api/stations* (список, додати, змінити, видалити,
// перемістити, імпорт, експорт). handleSettingsBody() перейменовано на
// handleJsonBody() (тепер спільний для всіх JSON-POST/PUT); логіку не змінено.

#include "net/web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ctype.h>
#include <esp_timer.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <new>

#include "audio/audio_processor.h"
#include "config/defaults.h"
#include "config/web_server_config.h"
#include "core/app_controller.h"
#include "core/app_state.h"
#include "core/settings.h"
#include "net/wifi_manager.h"
#include "stations/station_store.h"  // [Prompt 14]
#include "ui/display.h"

#if WEB_SERVER_DEBUG
#define WEB_LOG(fmt, ...) Serial.printf("[WEB] " fmt "\n", ##__VA_ARGS__)
#else
#define WEB_LOG(...) ((void)0)
#endif

namespace {

static_assert(sizeof(Settings::inputNames) / sizeof(Settings::inputNames[0]) ==
                  defaults::kInputCount,
              "Settings::inputNames must have defaults::kInputCount entries");
static_assert(sizeof(Settings::inputNames[0]) == web_cfg::kInputNameMaxBytes + 1,
              "web_cfg::kInputNameMaxBytes must match Settings::inputNames[i] size - 1");

constexpr uint8_t kProcTda7318 = 0;  // = AudioProcType::Tda7318 (settings.cpp static_assert)
constexpr uint8_t kProcPt2313l = 1;  // = AudioProcType::Pt2313l

AudioProcessor* s_proc = nullptr;
// Тип чипа, з яким прошивка ЗАРАЗ працює (Settings на момент begin()). Settings
// може змінитись через POST, але живий драйвер лишається старим до перезапуску.
uint8_t s_runningProcType = kProcTda7318;
AsyncWebServer* s_server = nullptr;
bool s_beginCalled = false;

// ---------------------------------------------------------------------------
// Імена enum'ів
// ---------------------------------------------------------------------------
const char* modeName(Mode m) {
    switch (m) {
        case Mode::Standby:       return "Standby";
        case Mode::Radio:         return "Radio";
        case Mode::ExternalInput: return "ExternalInput";
        case Mode::Menu:          return "Menu";
        case Mode::IrLearn:       return "IrLearn";
        case Mode::WifiSetup:     return "WifiSetup";
    }
    return "Unknown";
}

const char* streamStatusName(StreamStatus s) {
    switch (s) {
        case StreamStatus::Idle:         return "Idle";
        case StreamStatus::Connecting:   return "Connecting";
        case StreamStatus::Buffering:    return "Buffering";
        case StreamStatus::Playing:      return "Playing";
        case StreamStatus::Error:        return "Error";
        case StreamStatus::Reconnecting: return "Reconnecting";
    }
    return "Unknown";
}

const char* procTypeName(uint8_t t) {
    switch (t) {
        case kProcTda7318: return "Tda7318";
        case kProcPt2313l: return "Pt2313l";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Відповіді
// ---------------------------------------------------------------------------
void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
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

void sendError(AsyncWebServerRequest* req, int code, const char* error) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = error;
    sendJson(req, code, doc);
}

// ---------------------------------------------------------------------------
// GET /api/status
// ---------------------------------------------------------------------------
void handleStatus(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    const AppStateData st = AppState::snapshot();
    const Settings cfg = SettingsStore::snapshot();

    char ssid[33];
    char ip[16];
    WifiManager::copyInfo(ssid, sizeof(ssid), ip, sizeof(ip));

    // Назва входу: користувацька з Settings, якщо задана, інакше типова.
    const char* inputName = "";
    if (st.inputIndex < defaults::kInputCount) {
        inputName = cfg.inputNames[st.inputIndex];
        if (inputName[0] == '\0') inputName = defaults::kInputNames[st.inputIndex];
    }

    // Тонкомпенсація: фактичний запит до чипа, якщо він є; інакше з Settings.
    bool loudness = cfg.loudness;
    if (s_proc != nullptr) loudness = s_proc->cachedState().loudness;

    JsonDocument doc;
    doc["mode"] = modeName(st.mode);
    doc["input"] = st.inputIndex;
    doc["inputName"] = inputName;
    doc["volume"] = st.volume;
    doc["bass"] = st.bass;
    doc["treble"] = st.treble;
    doc["balance"] = st.balance;
    doc["gain"] = st.gain;
    doc["mute"] = st.mute;
    doc["loudness"] = loudness;

    JsonObject station = doc["station"].to<JsonObject>();
    station["index"] = st.stationIndex;
    station["name"] = st.stationName;
    doc["track"] = st.trackTitle;
    doc["streamStatus"] = streamStatusName(st.streamStatus);

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["connected"] = WifiManager::isConnected();
    wifi["apMode"] = WifiManager::isApMode();
    wifi["ssid"] = ssid;
    wifi["ip"] = ip;
    wifi["rssi"] = WifiManager::rssi();

    JsonObject proc = doc["processor"].to<JsonObject>();
    proc["type"] = procTypeName(s_runningProcType);
    proc["ready"] = (s_proc != nullptr);
    if (s_proc != nullptr) {
        const AudioProcessorCapabilities c = s_proc->capabilities();
        proc["i2cErrors"] = s_proc->i2cErrorCount();
        JsonObject caps = proc["capabilities"].to<JsonObject>();
        caps["bass"] = c.bass;
        caps["treble"] = c.treble;
        caps["balance"] = c.balance;
        caps["loudness"] = c.loudness;
        caps["inputCount"] = c.inputCount;
        caps["volumeMin"] = c.volumeMin;
        caps["volumeMax"] = c.volumeMax;
        caps["toneMin"] = c.toneMin;
        caps["toneMax"] = c.toneMax;
        caps["fader"] = c.fader;
        caps["inputGain"] = c.inputGain;
        caps["balanceMin"] = c.balanceMin;
        caps["balanceMax"] = c.balanceMax;
        caps["gainMin"] = c.gainMin;
        caps["gainMax"] = c.gainMax;
    } else {
        proc["capabilities"] = nullptr;
    }

    doc["brightness"] = DisplayManager::brightness();
    doc["displayFlipped"] = DisplayManager::isFlipped();
    doc["heap"] = ESP.getFreeHeap();
    doc["psramFree"] = ESP.getFreePsram();
    doc["uptimeMs"] = static_cast<uint64_t>(esp_timer_get_time() / 1000);

    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// GET /api/settings
// ---------------------------------------------------------------------------
void handleSettingsGet(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    const Settings s = SettingsStore::snapshot();

    JsonDocument doc;
    doc["processorType"] = procTypeName(s.processorType);
    JsonArray names = doc["inputNames"].to<JsonArray>();
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) names.add(s.inputNames[i]);
    doc["brightness"] = s.brightness;
    doc["displayFlipped"] = s.displayFlipped;
    doc["bass"] = s.bass;
    doc["treble"] = s.treble;
    doc["balance"] = s.balance;
    doc["loudness"] = s.loudness;
    doc["lastInput"] = s.lastInput;
    doc["lastStation"] = s.lastStation;
    doc["lastVolume"] = s.lastVolume;
    doc["lastMute"] = s.lastMute;

    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// Приймання малого JSON-тіла (POST /api/settings, /api/stations*)
// ---------------------------------------------------------------------------
// Тіло збираємо у malloc-буфер, який AsyncWebServerRequest сам звільняє
// (_tempObject) разом із запитом. Завеликі тіла не буферизуємо — відповідь 413
// дає handleSettingsPost() за Content-Length.
void handleJsonBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
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

// ---------------------------------------------------------------------------
// POST /api/settings — розбір і валідація
// ---------------------------------------------------------------------------
constexpr uint8_t kNames = defaults::kInputCount;

// Перевірений план змін. Нічого не застосовується, поки не пройшла ВСЯ валідація.
struct Plan {
    bool hasBrightness = false;
    uint8_t brightness = 0;
    bool hasFlipped = false;
    bool flipped = false;
    bool hasBass = false;
    int8_t bass = 0;
    bool hasTreble = false;
    int8_t treble = 0;
    bool hasBalance = false;
    int8_t balance = 0;
    bool hasLoudness = false;
    bool loudness = false;
    bool hasProc = false;
    uint8_t proc = 0;
    bool hasName[kNames] = {};
    char names[kNames][web_cfg::kInputNameMaxBytes + 1] = {};
};

struct ParseFlags {
    bool invalid = false;      // помилка клієнта -> 400
    bool unavailable = false;  // немає процесора -> 503 (якщо інших помилок нема)
    int fields = 0;            // скільки ключів прийнято
};

void addErr(JsonArray errs, const char* field, const char* code) {
    JsonObject o = errs.add<JsonObject>();
    o["field"] = field;
    o["code"] = code;
}

void addRangeErr(JsonArray errs, const char* field, int lo, int hi) {
    JsonObject o = errs.add<JsonObject>();
    o["field"] = field;
    o["code"] = "out_of_range";
    o["min"] = lo;
    o["max"] = hi;
}

// Ціле в межах [lo, hi]. Дробові та нечислові значення — "invalid_value".
bool readInt(JsonVariantConst v, const char* field, int lo, int hi, int& out, JsonArray errs,
             ParseFlags& f) {
    if (!v.is<int>()) {
        addErr(errs, field, "invalid_value");
        f.invalid = true;
        return false;
    }
    const int x = v.as<int>();
    if (x < lo || x > hi) {
        addRangeErr(errs, field, lo, hi);
        f.invalid = true;
        return false;
    }
    out = x;
    return true;
}

bool readBool(JsonVariantConst v, const char* field, bool& out, JsonArray errs, ParseFlags& f) {
    if (!v.is<bool>()) {
        addErr(errs, field, "invalid_value");
        f.invalid = true;
        return false;
    }
    out = v.as<bool>();
    return true;
}

// Назва входу: валідний UTF-8 без керувальних символів.
bool isCleanUtf8(const char* s) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
    while (*p != 0) {
        const uint8_t c = *p++;
        int tail;
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) tail = 0;
        else if (c >= 0xC2 && c <= 0xDF) tail = 1;
        else if ((c & 0xF0) == 0xE0) tail = 2;
        else if (c >= 0xF0 && c <= 0xF4) tail = 3;
        else return false;
        for (int i = 0; i < tail; ++i) {
            if ((*p & 0xC0) != 0x80) return false;
            ++p;
        }
    }
    return true;
}

void parsePlan(JsonObjectConst root, Plan& plan, JsonArray errs, ParseFlags& f) {
    AudioProcessorCapabilities caps = {};
    if (s_proc != nullptr) caps = s_proc->capabilities();

    for (JsonPairConst kv : root) {
        const char* key = kv.key().c_str();
        JsonVariantConst v = kv.value();
        int x = 0;

        if (strcmp(key, "brightness") == 0) {
            if (readInt(v, "brightness", web_cfg::kBrightnessMin, web_cfg::kBrightnessMax, x,
                        errs, f)) {
                plan.hasBrightness = true;
                plan.brightness = static_cast<uint8_t>(x);
                ++f.fields;
            }
        } else if (strcmp(key, "displayFlipped") == 0) {
            if (readBool(v, "displayFlipped", plan.flipped, errs, f)) {
                plan.hasFlipped = true;
                ++f.fields;
            }
        } else if (strcmp(key, "bass") == 0 || strcmp(key, "treble") == 0) {
            const bool isBass = (key[0] == 'b');
            const char* field = isBass ? "bass" : "treble";
            if (s_proc == nullptr) {
                addErr(errs, field, "audio_unavailable");
                f.unavailable = true;
            } else if (!(isBass ? caps.bass : caps.treble)) {
                addErr(errs, field, "not_supported");
                f.invalid = true;
            } else if (readInt(v, field, caps.toneMin, caps.toneMax, x, errs, f)) {
                if (isBass) {
                    plan.hasBass = true;
                    plan.bass = static_cast<int8_t>(x);
                } else {
                    plan.hasTreble = true;
                    plan.treble = static_cast<int8_t>(x);
                }
                ++f.fields;
            }
        } else if (strcmp(key, "balance") == 0) {
            if (s_proc == nullptr) {
                addErr(errs, "balance", "audio_unavailable");
                f.unavailable = true;
            } else if (!caps.balance) {
                addErr(errs, "balance", "not_supported");
                f.invalid = true;
            } else if (readInt(v, "balance", caps.balanceMin, caps.balanceMax, x, errs, f)) {
                plan.hasBalance = true;
                plan.balance = static_cast<int8_t>(x);
                ++f.fields;
            }
        } else if (strcmp(key, "loudness") == 0) {
            if (s_proc == nullptr) {
                addErr(errs, "loudness", "audio_unavailable");
                f.unavailable = true;
            } else if (!caps.loudness) {
                addErr(errs, "loudness", "not_supported");
                f.invalid = true;
            } else if (readBool(v, "loudness", plan.loudness, errs, f)) {
                plan.hasLoudness = true;
                ++f.fields;
            }
        } else if (strcmp(key, "processorType") == 0) {
            const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
            if (s != nullptr && strcmp(s, "Tda7318") == 0) {
                plan.proc = kProcTda7318;
            } else if (s != nullptr && strcmp(s, "Pt2313l") == 0) {
                plan.proc = kProcPt2313l;
            } else {
                addErr(errs, "processorType", "invalid_value");
                f.invalid = true;
                continue;
            }
            plan.hasProc = true;
            ++f.fields;
        } else if (strcmp(key, "inputNames") == 0) {
            if (!v.is<JsonArrayConst>() || v.as<JsonArrayConst>().size() != kNames) {
                addErr(errs, "inputNames", "expected_array_of_4");
                f.invalid = true;
                continue;
            }
            JsonArrayConst arr = v.as<JsonArrayConst>();
            bool any = false;
            for (uint8_t i = 0; i < kNames; ++i) {
                JsonVariantConst e = arr[i];
                if (e.isNull()) continue;  // null = «не змінювати»
                const char* s = e.is<const char*>() ? e.as<const char*>() : nullptr;
                const size_t n = (s != nullptr) ? strlen(s) : 0;
                if (s == nullptr || n < 1 || n > web_cfg::kInputNameMaxBytes || !isCleanUtf8(s)) {
                    JsonObject o = errs.add<JsonObject>();
                    o["field"] = "inputNames";
                    o["code"] = "invalid_name";
                    o["index"] = i;
                    o["maxBytes"] = web_cfg::kInputNameMaxBytes;
                    f.invalid = true;
                    continue;
                }
                memcpy(plan.names[i], s, n + 1);
                plan.hasName[i] = true;
                any = true;
            }
            if (any) ++f.fields;
        } else {
            // Невідоме поле — відхиляємо, а не ігноруємо мовчки (друкарська помилка
            // у клієнта не має виглядати як успіх). Покажчик на ключ живе, доки
            // живе документ запиту.
            JsonObject o = errs.add<JsonObject>();
            o["field"] = key;
            o["code"] = "unknown_field";
            f.invalid = true;
        }
    }
}

// ---------------------------------------------------------------------------
// POST /api/settings — застосування
// ---------------------------------------------------------------------------
struct SettingsCtx {
    const Plan* plan;
    bool saveLoudness;  // тембр/баланс зберігає AppController::setTone()
};

void handleSettingsPost(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s (%u bytes)", req->methodToString(), req->url().c_str(),
            static_cast<unsigned>(req->contentLength()));

    if (req->contentLength() > web_cfg::kMaxBodyBytes) {
        sendError(req, 413, "body_too_large");
        return;
    }
    const char* body = static_cast<const char*>(req->_tempObject);
    if (req->contentLength() == 0 || body == nullptr) {
        sendError(req, 400, "no_body");
        return;
    }

    JsonDocument in;
    const DeserializationError de = deserializeJson(in, body, req->contentLength());
    if (de || !in.is<JsonObject>()) {
        sendError(req, 400, "invalid_json");
        return;
    }

    JsonDocument resp;
    JsonArray errs = resp["errors"].to<JsonArray>();
    Plan plan;
    ParseFlags flags;
    if (in.as<JsonObjectConst>().size() == 0) {
        addErr(errs, "", "empty_request");
        flags.invalid = true;
    } else {
        parsePlan(in.as<JsonObjectConst>(), plan, errs, flags);
    }

    // Атомарно: будь-яка помилка валідації -> нічого не застосовано.
    if (flags.invalid || flags.unavailable) {
        resp["ok"] = false;
        resp.remove("results");
        sendJson(req, flags.invalid ? 400 : 503, resp);
        return;
    }
    resp.remove("errors");

    JsonObject results = resp["results"].to<JsonObject>();
    JsonArray restart = resp["requiresRestart"].to<JsonArray>();
    bool hwFailed = false;

    // --- Дисплей (безпечно з будь-якої задачі) ---
    if (plan.hasBrightness) {
        DisplayManager::setBrightness(plan.brightness);
        results["brightness"]["applied"] = true;
    }
    if (plan.hasFlipped) {
        DisplayManager::setFlipped(plan.flipped);
        results["displayFlipped"]["applied"] = true;
    }

    // --- Тембр/баланс: через AppController::setTone() ---
    // AppController тримає власні копії bass/treble/balance і публікує їх у AppState та
    // Settings після кожної події, тому пряма зміна чипа була б затерта. setTone() під
    // його мʼютексом: чип -> копія -> AppState -> Settings.
    SettingsCtx sc = {&plan, plan.hasLoudness};
    if (plan.hasBass || plan.hasTreble || plan.hasBalance) {
        ToneUpdate tu;
        tu.hasBass = plan.hasBass;       tu.bass = plan.bass;
        tu.hasTreble = plan.hasTreble;   tu.treble = plan.treble;
        tu.hasBalance = plan.hasBalance; tu.balance = plan.balance;
        const bool handled = AppController::setTone(tu);
        auto toneResult = [&](const char* field, bool wanted, bool ok) {
            if (!wanted) return;
            if (ok) {
                results[field]["applied"] = true;
            } else {
                results[field]["applied"] = false;
                results[field]["error"] = handled ? "i2c_failed" : "busy";
                hwFailed = true;
            }
        };
        toneResult("bass", plan.hasBass, tu.bassOk);
        toneResult("treble", plan.hasTreble, tu.trebleOk);
        toneResult("balance", plan.hasBalance, tu.balanceOk);
    }

    // --- Тонкомпенсація: AppController її не веде, у AppState поля немає ---
    if (plan.hasLoudness) {
        if (s_proc->setLoudness(plan.loudness)) {
            results["loudness"]["applied"] = true;
        } else {
            results["loudness"]["applied"] = false;
            results["loudness"]["error"] = "i2c_failed";
            sc.saveLoudness = false;
            hwFailed = true;
        }
    }

    // --- Settings (один modify = один мʼютекс, один requestSave) ---
    if (plan.hasProc) {
        if (plan.proc == s_runningProcType) {
            results["processorType"]["applied"] = true;
        } else {
            results["processorType"]["applied"] = false;
            results["processorType"]["requiresRestart"] = true;
            restart.add("processorType");
        }
    }
    bool anyName = false;
    for (uint8_t i = 0; i < kNames; ++i) anyName = anyName || plan.hasName[i];
    if (anyName) results["inputNames"]["applied"] = true;

    const bool touchSettings = plan.hasBrightness || plan.hasFlipped || plan.hasProc || anyName ||
                               sc.saveLoudness;
    if (touchSettings) {
        SettingsStore::modify(
            [](Settings& s, void* c) {
                const SettingsCtx* x = static_cast<const SettingsCtx*>(c);
                const Plan& p = *x->plan;
                if (p.hasBrightness) s.brightness = p.brightness;
                if (p.hasFlipped) s.displayFlipped = p.flipped;
                if (x->saveLoudness) s.loudness = p.loudness;
                if (p.hasProc) s.processorType = p.proc;
                for (uint8_t i = 0; i < kNames; ++i) {
                    if (p.hasName[i]) memcpy(s.inputNames[i], p.names[i], sizeof(s.inputNames[i]));
                }
            },
            &sc);
        SettingsStore::requestSave();
    }

    resp["ok"] = !hwFailed;
    WEB_LOG("settings: %d field(s), hwFailed=%d", flags.fields, hwFailed ? 1 : 0);
    sendJson(req, hwFailed ? 502 : 200, resp);
}

void handleNotFound(AsyncWebServerRequest* req) {
    WEB_LOG("404 %s %s", req->methodToString(), req->url().c_str());
    sendError(req, 404, "not_found");
}

// ---------------------------------------------------------------------------
// [Prompt 14] /api/stations*
// ---------------------------------------------------------------------------
constexpr const char* kStationsPath = "/api/stations";
constexpr const char* kStationsPrefix = "/api/stations/";

void sendFieldError(AsyncWebServerRequest* req, int code, const char* error, const char* field) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = error;
    if (field != nullptr) doc["field"] = field;
    sendJson(req, code, doc);
}

// Читає й розбирає JSON-обʼєкт із тіла. При помилці сам шле відповідь і false.
bool readJsonObjectBody(AsyncWebServerRequest* req, JsonDocument& in) {
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

bool hasNonSpace(const char* s) {
    for (; *s != '\0'; ++s) {
        if (!isspace(static_cast<unsigned char>(*s))) return true;
    }
    return false;
}

// {"name","url"} -> Station. Обидва поля обовʼязкові, інших ключів немає.
// nullptr — успіх; інакше код помилки й *badField (ім'я поля чи nullptr).
const char* parseStationObject(JsonObjectConst root, Station& out, const char** badField) {
    *badField = nullptr;
    for (JsonPairConst kv : root) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "name") != 0 && strcmp(k, "url") != 0) {
            *badField = "unknown";
            return "unknown_field";
        }
    }
    JsonVariantConst vn = root["name"];
    JsonVariantConst vu = root["url"];

    const char* name = vn.is<const char*>() ? vn.as<const char*>() : nullptr;
    if (name == nullptr || !hasNonSpace(name)) {
        *badField = "name";
        return "name_required";
    }
    if (strlen(name) > station_store_cfg::kNameMax - 1) {
        *badField = "name";
        return "name_too_long";
    }
    if (!isCleanUtf8(name)) {
        *badField = "name";
        return "name_invalid";
    }

    const char* url = vu.is<const char*>() ? vu.as<const char*>() : nullptr;
    if (url == nullptr || !hasNonSpace(url)) {
        *badField = "url";
        return "url_required";
    }
    if (strlen(url) > station_store_cfg::kUrlMax - 1) {
        *badField = "url";
        return "url_too_long";
    }
    if (!isCleanUtf8(url)) {
        *badField = "url";
        return "url_invalid";
    }
    const char* u = url;
    while (*u != '\0' && isspace(static_cast<unsigned char>(*u))) ++u;
    if (strncasecmp(u, "http://", 7) != 0 && strncasecmp(u, "https://", 8) != 0) {
        *badField = "url";
        return "url_invalid_scheme";
    }

    memset(&out, 0, sizeof(out));
    strlcpy(out.name, name, sizeof(out.name));
    strlcpy(out.url, url, sizeof(out.url));
    return nullptr;
}

// "/api/stations/{index}" -> index. false — не число, порожньо чи задовге.
bool parseStationIndex(const String& url, size_t& out) {
    if (!url.startsWith(kStationsPrefix)) return false;
    const char* p = url.c_str() + strlen(kStationsPrefix);
    const size_t len = strlen(p);
    if (len == 0 || len > web_cfg::kStationIndexMaxDigits) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit(static_cast<unsigned char>(p[i]))) return false;
    }
    out = static_cast<size_t>(strtoul(p, nullptr, 10));
    return true;
}

// Захист від перетину маршрутів: ESPAsyncWebServer зіставляє "/api/stations" і
// з "/api/stations/...". Обробники точного шляху перевіряють url самі.
bool exactStationsUrl(AsyncWebServerRequest* req) {
    if (req->url() == kStationsPath) return true;
    handleNotFound(req);
    return false;
}

// GET /api/stations -> [{"index":0,"name":"..","url":".."},..]
// Пишемо елемент за елементом (без одного великого JsonDocument на весь список).
void handleStationsList(AsyncWebServerRequest* req) {
    if (!exactStationsUrl(req)) return;
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    AsyncResponseStream* r = req->beginResponseStream("application/json");
    if (r == nullptr) {
        req->send(503);
        return;
    }
    r->addHeader("Cache-Control", "no-store");
    r->print("[");
    const size_t n = StationStore::count();
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        Station st;
        if (!StationStore::get(i, st)) continue;  // список скоротився під час відповіді
        if (!first) r->print(",");
        first = false;
        JsonDocument d;
        d["index"] = i;
        d["name"] = st.name;
        d["url"] = st.url;
        serializeJson(d, *r);
    }
    r->print("]");
    req->send(r);
}

// POST /api/stations {"name","url"} -> 201 {"ok":true,"index":N}
void handleStationsAdd(AsyncWebServerRequest* req) {
    if (!exactStationsUrl(req)) return;
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;

    Station st;
    const char* field = nullptr;
    const char* err = parseStationObject(in.as<JsonObjectConst>(), st, &field);
    if (err != nullptr) {
        sendFieldError(req, 400, err, field);
        return;
    }
    size_t idx = 0;
    if (!StationStore::add(st, &idx)) {
        if (StationStore::count() >= station_store_cfg::kMaxStations) {
            sendError(req, 409, "list_full");
        } else {
            sendError(req, 500, "storage_error");
        }
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["index"] = idx;
    sendJson(req, 201, doc);
}

// PUT /api/stations/{index} {"name","url"} (повна заміна) -> 200 {"ok":true,"index":N}
void handleStationsUpdate(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    size_t idx = 0;
    if (!parseStationIndex(req->url(), idx)) {
        sendError(req, 400, "invalid_index");
        return;
    }
    if (idx >= StationStore::count()) {
        sendError(req, 404, "not_found");
        return;
    }
    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;

    Station st;
    const char* field = nullptr;
    const char* err = parseStationObject(in.as<JsonObjectConst>(), st, &field);
    if (err != nullptr) {
        sendFieldError(req, 400, err, field);
        return;
    }
    if (!StationStore::update(idx, st)) {
        if (idx >= StationStore::count()) {
            sendError(req, 404, "not_found");  // список скоротився між перевіркою й записом
        } else {
            sendError(req, 500, "storage_error");
        }
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["index"] = idx;
    sendJson(req, 200, doc);
}

// DELETE /api/stations/{index} -> 200 {"ok":true}
void handleStationsDelete(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    size_t idx = 0;
    if (!parseStationIndex(req->url(), idx)) {
        sendError(req, 400, "invalid_index");
        return;
    }
    if (idx >= StationStore::count()) {
        sendError(req, 404, "not_found");
        return;
    }
    if (!StationStore::remove(idx)) {
        if (idx >= StationStore::count()) {
            sendError(req, 404, "not_found");
        } else {
            sendError(req, 500, "storage_error");
        }
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
}

// POST /api/stations/move {"from":N,"to":M} -> 200 {"ok":true}
void handleStationsMove(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;

    JsonObjectConst root = in.as<JsonObjectConst>();
    for (JsonPairConst kv : root) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "from") != 0 && strcmp(k, "to") != 0) {
            sendFieldError(req, 400, "unknown_field", "unknown");
            return;
        }
    }
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
    const size_t n = StationStore::count();
    if (from < 0 || static_cast<size_t>(from) >= n) {
        sendFieldError(req, 400, "index_out_of_range", "from");
        return;
    }
    if (to < 0 || static_cast<size_t>(to) >= n) {
        sendFieldError(req, 400, "index_out_of_range", "to");
        return;
    }
    if (!StationStore::move(static_cast<size_t>(from), static_cast<size_t>(to))) {
        const size_t now = StationStore::count();
        if (static_cast<size_t>(from) >= now || static_cast<size_t>(to) >= now) {
            sendError(req, 400, "index_out_of_range");
        } else {
            sendError(req, 500, "storage_error");
        }
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
}

// --- POST /api/stations/import -------------------------------------------
// Тіло — сирий вміст файлу. Складаємо його прямо у тимчасовий файл LittleFS (до
// 128 КБ у RAM не тримаємо). Станом володіє одна «активна» заявка; друга
// одночасна отримає 409. Стан — статичний, бо req->_tempObject звільняється
// через free() і не годиться для обʼєкта з File.
struct ImportCtx {
    File file;
    AsyncWebServerRequest* owner = nullptr;
    bool failed = false;
    size_t written = 0;
};
ImportCtx s_imp;

void importRelease() {
    if (s_imp.file) s_imp.file.close();
    LittleFS.remove(web_cfg::kStationsImportTmpPath);
    s_imp.owner = nullptr;
    s_imp.failed = false;
    s_imp.written = 0;
}

void handleImportBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
                      size_t total) {
    if (total == 0 || total > web_cfg::kStationsImportMaxBytes) return;  // відповість handler
    if (index == 0) {
        if (s_imp.owner != nullptr) return;  // зайнято іншою заявкою -> 409 у handler
        LittleFS.remove(web_cfg::kStationsImportTmpPath);
        s_imp.file = LittleFS.open(web_cfg::kStationsImportTmpPath, "w");
        s_imp.owner = req;
        s_imp.failed = !s_imp.file;
        s_imp.written = 0;
        // Обрив зʼєднання посеред завантаження: звільнити стан і файл.
        req->onDisconnect([req]() {
            if (s_imp.owner == req) importRelease();
        });
    }
    if (s_imp.owner != req || s_imp.failed) return;
    if (s_imp.file.write(data, len) != len) {
        s_imp.failed = true;
        return;
    }
    s_imp.written += len;
}

// Формат: ?format=m3u|pls|json (головне), інакше за Content-Type.
// nullptr — не визначено.
const char* detectImportFormat(AsyncWebServerRequest* req) {
    if (req->hasParam("format")) {
        const String f = req->getParam("format")->value();
        if (f.equalsIgnoreCase("m3u")) return "m3u";
        if (f.equalsIgnoreCase("pls")) return "pls";
        if (f.equalsIgnoreCase("json")) return "json";
        return nullptr;  // явно вказано, але невідомо — не вгадуємо
    }
    String ct = req->contentType();
    ct.toLowerCase();
    if (ct.startsWith("application/json")) return "json";
    if (ct.startsWith("audio/x-scpls") || ct.startsWith("audio/scpls")) return "pls";
    if (ct.startsWith("audio/x-mpegurl") || ct.startsWith("audio/mpegurl") ||
        ct.startsWith("application/vnd.apple.mpegurl") ||
        ct.startsWith("application/x-mpegurl")) {
        return "m3u";
    }
    return nullptr;
}

void handleStationsImport(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s (%u bytes)", req->methodToString(), req->url().c_str(),
            static_cast<unsigned>(req->contentLength()));

    const size_t total = req->contentLength();
    if (total > web_cfg::kStationsImportMaxBytes) {
        sendError(req, 413, "body_too_large");
        return;
    }
    if (total == 0) {
        sendError(req, 400, "no_body");
        return;
    }
    if (s_imp.owner != req) {
        sendError(req, 409, "import_busy");
        return;
    }
    if (s_imp.file) s_imp.file.close();
    if (s_imp.failed || s_imp.written != total) {
        importRelease();
        sendError(req, 500, "upload_failed");
        return;
    }

    const char* fmt = detectImportFormat(req);
    if (fmt == nullptr) {
        importRelease();
        sendError(req, 400, "unknown_format");
        return;
    }

    const char* path = web_cfg::kStationsImportTmpPath;
    bool ok = false;
    if (strcmp(fmt, "m3u") == 0) {
        ok = StationStore::importM3u(path);
    } else if (strcmp(fmt, "pls") == 0) {
        ok = StationStore::importPls(path);
    } else {
        ok = StationStore::importJson(path);
    }
    const char* why = StationStore::lastImportError();
    importRelease();  // видаляє тимчасовий файл

    if (!ok) {
        int code = 400;  // помилка вмісту
        if (strcmp(why, "busy") == 0) {
            code = 503;
        } else if (strcmp(why, "out_of_memory") == 0 || strcmp(why, "io_error") == 0 ||
                   strcmp(why, "storage_write_failed") == 0 ||
                   strcmp(why, "file_not_found") == 0) {
            code = 500;
        }
        WEB_LOG("stations import (%s) failed: %s", fmt, why);
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"] = "import_failed";
        doc["reason"] = why;
        doc["format"] = fmt;
        sendJson(req, code, doc);
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["format"] = fmt;
    doc["count"] = StationStore::count();
    doc["replaced"] = true;
    sendJson(req, 200, doc);
}

// GET /api/stations/export -> файл (Content-Disposition: attachment)
void handleStationsExport(AsyncWebServerRequest* req) {
    WEB_LOG("%s %s", req->methodToString(), req->url().c_str());

    if (!StationStore::exportJson(web_cfg::kStationsExportPath)) {
        sendError(req, 500, "export_failed");
        return;
    }
    AsyncWebServerResponse* r =
        req->beginResponse(LittleFS, web_cfg::kStationsExportPath, "application/json");
    if (r == nullptr) {
        sendError(req, 500, "export_failed");
        return;
    }
    char cd[96];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", web_cfg::kStationsExportFilename);
    r->addHeader("Content-Disposition", cd);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

// ---------------------------------------------------------------------------
// Старт сервера
// ---------------------------------------------------------------------------
void registerRoutes(AsyncWebServer& server) {
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/settings", HTTP_GET, handleSettingsGet);
    server.on("/api/settings", HTTP_POST, handleSettingsPost, nullptr, handleJsonBody);

    // [Prompt 14] ПОРЯДОК ВАЖЛИВИЙ: ESPAsyncWebServer вважає "/api/stations" збігом і
    // для "/api/stations/...", а перший збіг за методом виграє. Тому спершу точні
    // підшляхи, потім "/api/stations/*" (PUT/DELETE за індексом), і наприкінці —
    // "/api/stations" (його обробники ще й перевіряють url самі).
    server.on("/api/stations/move", HTTP_POST, handleStationsMove, nullptr, handleJsonBody);
    server.on("/api/stations/import", HTTP_POST, handleStationsImport, nullptr, handleImportBody);
    server.on("/api/stations/export", HTTP_GET, handleStationsExport);
    server.on("/api/stations/*", HTTP_PUT, handleStationsUpdate, nullptr, handleJsonBody);
    server.on("/api/stations/*", HTTP_DELETE, handleStationsDelete);
    server.on("/api/stations", HTTP_GET, handleStationsList);
    server.on("/api/stations", HTTP_POST, handleStationsAdd, nullptr, handleJsonBody);

    server.onNotFound(handleNotFound);
}

// Одноразова задача: чекає на STA з IP, піднімає сервер і видаляє себе.
// У режимі AP (captive portal займає порт 80) не стартує; перехід AP -> STA
// відбувається лише перезапуском (Prompt 12), після якого begin() викликається
// знову. Якщо колись AP -> STA стане «на льоту», задача просто дочекається.
void starterTask(void*) {
    for (;;) {
        if (WifiManager::isConnected() && !WifiManager::isApMode()) break;
        vTaskDelay(pdMS_TO_TICKS(web_cfg::kStartPollMs));
    }

    s_server = new (std::nothrow) AsyncWebServer(web_cfg::kHttpPort);
    if (s_server == nullptr) {
        Serial.println("[WEB] ERROR: no memory for AsyncWebServer");
        vTaskDelete(nullptr);
        return;
    }
    registerRoutes(*s_server);
    s_server->begin();
    Serial.printf("[WEB] HTTP server listening on :%u (http://%s/)\n",
                  static_cast<unsigned>(web_cfg::kHttpPort), WifiManager::ipAddress());
    vTaskDelete(nullptr);
}

}  // namespace

bool WebServerManager::begin(AudioProcessor* processorOrNull) {
    if (s_beginCalled) return true;

    s_proc = processorOrNull;
    s_runningProcType = SettingsStore::snapshot().processorType;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        starterTask, "web_start", web_cfg::kStarterStackBytes, nullptr,
        web_cfg::kStarterTaskPriority, nullptr, web_cfg::kStarterTaskCore);
    if (ok != pdPASS) return false;

    s_beginCalled = true;
    return true;
}
