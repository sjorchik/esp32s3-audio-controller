// net/web_api_player.cpp (Prompt 18): POST /api/power, /api/mute, /api/volume, /api/gain,
// /api/input, /api/player/*, /api/player/station.
// [Prompt 30] + GET/POST /api/eq (5-смуговий еквалайзер радіо). На відміну від решти, /api/eq НЕ
// йде через AppController::runWebCommand(): це чисто програмний фільтр у хуку PCM (audio/eq.*),
// без заліза й без стану контролера; обробник кладе значення в eq::* і в Settings (NVS, дебаунс).
//
// Усе, що міняє стан пристрою, виконує AppController::runWebCommand() під його мʼютексом
// (те саме, що кнопки/пульт: ramp, мʼют на переходах, збереження в Settings). Тут лише:
// розбір і валідація тіла, перевірка меж за capabilities(), мапінг WebCmdResult -> HTTP-код.
//
// [Prompt 39] + GET/POST /api/inputs: доступність і відключення зовнішніх входів 1..3 (радіо — завжди
// дозволене). Зміна йде через AppController::runWebCommand(InputsEnabledSet), бо контролер має
// перемкнути активний вхід, якщо його вимкнено; сам обробник лише валідує тіло й будує відповідь.
//
// Коди: 200 ok | 400 помилка клієнта (інвалідне значення/поза межами/не підтримується) |
//       404 невідомий підшлях | 409 зараз недопустимо (standby, OTA, навчання IR, не вхід
//       Radio, немає станцій) | 503 контролер зайнятий або немає аудіопроцесора.

#include "net/web_api_player.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "audio/audio_processor.h"
#include "audio/eq.h"
#include "config/defaults.h"         // [Prompt 39]
#include "config/eq_config.h"
#include "config/settings_config.h"  // [Prompt 39] normalizeInputMask
#include "core/app_controller.h"
#include "core/app_state.h"
#include "core/settings.h"
#include "net/web_api_common.h"
#include "stations/station_store.h"

namespace {

using namespace web_api;

AudioProcessor* s_proc = nullptr;

// {"ok":true,"state":{...}} — компактний знімок ПІСЛЯ команди (контролер уже опублікував
// стан у AppState). streamStatus свідомо не віддаємо: він оновлюється періодично
// (app_controller_cfg::kSyncPeriodMs) і одразу після команди міг би бути застарілим —
// клієнт бере його з GET /api/status.
void sendOkState(AsyncWebServerRequest* req) {
    const AppStateData st = AppState::snapshot();
    JsonDocument doc;
    doc["ok"] = true;
    JsonObject s = doc["state"].to<JsonObject>();
    s["mode"] = modeName(st.mode);
    s["standby"] = (st.mode == Mode::Standby);
    s["input"] = st.inputIndex;
    s["volume"] = st.volume;
    s["mute"] = st.mute;
    s["stationIndex"] = st.stationIndex;
    sendJson(req, 200, doc);
}

void sendResult(AsyncWebServerRequest* req, WebCmdResult r, const char* why) {
    switch (r) {
        case WebCmdResult::Ok:
            sendOkState(req);
            return;
        case WebCmdResult::Busy:
            sendError(req, 503, "busy");
            return;
        case WebCmdResult::NotAllowed:
            sendError(req, 409, why != nullptr ? why : "not_allowed");
            return;
        case WebCmdResult::OutOfRange:
            sendError(req, 400, why != nullptr ? why : "out_of_range");
            return;
        case WebCmdResult::Unsupported:
            sendError(req, 400, "not_supported");
            return;
    }
    sendError(req, 500, "internal");
}

void runCommand(AsyncWebServerRequest* req, const WebCommand& c) {
    const char* why = nullptr;
    const WebCmdResult r = AppController::runWebCommand(c, &why);
    WEB_API_LOG("cmd %d -> result %d (%s)", static_cast<int>(c.type), static_cast<int>(r),
                why != nullptr ? why : "-");
    sendResult(req, r, why);
}

// Команди, що б'ють по залізу чипа: без процесора — 503 (як аудіополя POST /api/settings).
bool requireProc(AsyncWebServerRequest* req) {
    if (s_proc != nullptr) return true;
    sendError(req, 503, "audio_unavailable");
    return false;
}

// ---------------------------------------------------------------------------
// POST /api/power {"state":"on"|"off"|"toggle"}   on = вийти зі standby
// ---------------------------------------------------------------------------
void handlePower(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/power")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "state")) return;

    JsonVariantConst v = root["state"];
    const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
    WebCommand c = {WebCmdType::StandbyToggle, 0, false};
    if (s != nullptr && strcmp(s, "on") == 0) {
        c = {WebCmdType::StandbySet, 0, false};
    } else if (s != nullptr && strcmp(s, "off") == 0) {
        c = {WebCmdType::StandbySet, 0, true};
    } else if (s != nullptr && strcmp(s, "toggle") == 0) {
        // c вже StandbyToggle
    } else {
        sendFieldError(req, 400, "invalid_value", "state");
        return;
    }
    runCommand(req, c);
}

// ---------------------------------------------------------------------------
// POST /api/mute {"mute":true|false|"toggle"}   (мʼют атенюаторів, як утримання енкодера)
// ---------------------------------------------------------------------------
void handleMute(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/mute")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!requireProc(req)) return;

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "mute")) return;

    JsonVariantConst v = root["mute"];
    WebCommand c = {WebCmdType::MuteToggle, 0, false};
    if (v.is<bool>()) {
        c = {WebCmdType::MuteSet, 0, v.as<bool>()};
    } else if (v.is<const char*>() && strcmp(v.as<const char*>(), "toggle") == 0) {
        // c вже MuteToggle
    } else {
        sendFieldError(req, 400, "invalid_value", "mute");
        return;
    }
    runCommand(req, c);
}

// ---------------------------------------------------------------------------
// POST /api/volume {"value":N} (абсолютне, volumeMin..volumeMax) | {"step":N} (відносне)
// ---------------------------------------------------------------------------
void handleVolume(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/volume")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!requireProc(req)) return;

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "value", "step")) return;

    JsonVariantConst vv = root["value"];
    JsonVariantConst vs = root["step"];
    if (!vv.isNull() && !vs.isNull()) {
        sendFieldError(req, 400, "conflicting_fields", "value");
        return;
    }
    if (vv.isNull() && vs.isNull()) {
        sendFieldError(req, 400, "missing_field", "value");
        return;
    }
    const AudioProcessorCapabilities caps = s_proc->capabilities();

    if (!vv.isNull()) {
        if (!vv.is<int>()) {
            sendFieldError(req, 400, "invalid_value", "value");
            return;
        }
        const int x = vv.as<int>();
        if (x < caps.volumeMin || x > caps.volumeMax) {
            sendRangeError(req, "value", caps.volumeMin, caps.volumeMax);
            return;
        }
        runCommand(req, {WebCmdType::VolumeSet, x, false});
    } else {
        if (!vs.is<int>()) {
            sendFieldError(req, 400, "invalid_value", "step");
            return;
        }
        const int x = vs.as<int>();
        if (x < -web_api_cfg::kVolumeStepAbsMax || x > web_api_cfg::kVolumeStepAbsMax) {
            sendRangeError(req, "step", -web_api_cfg::kVolumeStepAbsMax,
                           web_api_cfg::kVolumeStepAbsMax);
            return;
        }
        runCommand(req, {WebCmdType::VolumeStep, x, false});
    }
}

// ---------------------------------------------------------------------------
// POST /api/gain {"value":N}   підсилення ПОТОЧНОГО входу, сирі кроки gainMin..gainMax
// ---------------------------------------------------------------------------
void handleGain(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/gain")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!requireProc(req)) return;

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "value")) return;

    const AudioProcessorCapabilities caps = s_proc->capabilities();
    if (!caps.inputGain || caps.gainMax <= caps.gainMin) {
        sendFieldError(req, 400, "not_supported", "value");
        return;
    }
    JsonVariantConst v = root["value"];
    if (!v.is<int>()) {
        sendFieldError(req, 400, "invalid_value", "value");
        return;
    }
    const int x = v.as<int>();
    if (x < caps.gainMin || x > caps.gainMax) {
        sendRangeError(req, "value", caps.gainMin, caps.gainMax);
        return;
    }
    runCommand(req, {WebCmdType::GainSet, x, false});
}

// ---------------------------------------------------------------------------
// POST /api/input {"index":N}   0..inputCount-1 (PT2313L: вхід 3 -> 400)
// ---------------------------------------------------------------------------
void handleInput(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/input")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    if (!requireProc(req)) return;

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "index")) return;

    JsonVariantConst v = root["index"];
    if (!v.is<int>()) {
        sendFieldError(req, 400, "invalid_value", "index");
        return;
    }
    const AudioProcessorCapabilities caps = s_proc->capabilities();
    const int x = v.as<int>();
    if (x < 0 || x >= caps.inputCount) {
        sendRangeError(req, "index", 0, static_cast<int>(caps.inputCount) - 1);
        return;
    }
    runCommand(req, {WebCmdType::InputSet, x, false});
}

// ---------------------------------------------------------------------------
// POST /api/player/{play|pause|stop|toggle|next|prev}   (без тіла)
// stop = pause (потік живий, позиції немає: AudioPlayer::stop() скасовує й перепідключення)
// ---------------------------------------------------------------------------
void handlePlayerAction(AsyncWebServerRequest* req) {
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    const String url = req->url();
    static const char kPrefix[] = "/api/player/";
    if (!url.startsWith(kPrefix)) {
        sendError(req, 404, "not_found");
        return;
    }
    const char* act = url.c_str() + (sizeof(kPrefix) - 1);

    WebCommand c = {WebCmdType::PlayerToggle, 0, false};
    if (strcmp(act, "play") == 0) {
        c.type = WebCmdType::PlayerPlay;
    } else if (strcmp(act, "pause") == 0 || strcmp(act, "stop") == 0) {
        c.type = WebCmdType::PlayerPause;
    } else if (strcmp(act, "toggle") == 0) {
        c.type = WebCmdType::PlayerToggle;
    } else if (strcmp(act, "next") == 0) {
        c.type = WebCmdType::StationNext;
    } else if (strcmp(act, "prev") == 0) {
        c.type = WebCmdType::StationPrev;
    } else {
        sendError(req, 404, "not_found");
        return;
    }
    runCommand(req, c);
}

// ---------------------------------------------------------------------------
// POST /api/player/station {"index":N[,"switchInput":true]}
// switchInput=true: якщо активний інший вхід — перейти на Radio і зіграти станцію.
// ---------------------------------------------------------------------------
void handlePlayerStation(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/player/station")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "index", "switchInput")) return;

    JsonVariantConst vi = root["index"];
    if (!vi.is<int>()) {
        sendFieldError(req, 400, "invalid_value", "index");
        return;
    }
    bool switchInput = false;
    JsonVariantConst vw = root["switchInput"];
    if (!vw.isNull()) {
        if (!vw.is<bool>()) {
            sendFieldError(req, 400, "invalid_value", "switchInput");
            return;
        }
        switchInput = vw.as<bool>();
    }
    const size_t n = StationStore::count();
    if (n == 0) {
        sendError(req, 409, "no_stations");
        return;
    }
    const int idx = vi.as<int>();
    if (idx < 0 || static_cast<size_t>(idx) >= n) {
        sendFieldError(req, 400, "index_out_of_range", "index");
        return;
    }
    runCommand(req, {WebCmdType::StationPlay, idx, switchInput});
}

// ---------------------------------------------------------------------------
// [Prompt 30] GET /api/eq, POST /api/eq — 5-смуговий еквалайзер радіо
//   GET : {"gainsDb":[0,0,0,0,0],"freqHz":[60,250,1000,4000,12000],"minDb":-12,"maxDb":12,"stepDb":1}
//   POST: {"gainsDb":[a,b,c,d,e]}  (рівно 5 елементів; null = смугу не чіпати)
//         або {"band":N,"gainDb":X}  (одна смуга, N = 0..4)
//         -> 200 {"ok":true, ...те саме, що GET}
// Пресет глобальний (не по входах і не по станціях) і діє лише на потік радіо.
// ---------------------------------------------------------------------------
void fillEqState(JsonDocument& doc) {
    int8_t g[eq_cfg::kBandCount];
    eq::getAllDb(g);
    JsonArray gains = doc["gainsDb"].to<JsonArray>();
    JsonArray freqs = doc["freqHz"].to<JsonArray>();
    for (uint8_t i = 0; i < eq_cfg::kBandCount; ++i) {
        gains.add(static_cast<int>(g[i]));
        freqs.add(static_cast<unsigned>(eq_cfg::kBandFreqHz[i]));
    }
    doc["minDb"] = eq_cfg::kGainMinDb;
    doc["maxDb"] = eq_cfg::kGainMaxDb;
    doc["stepDb"] = eq_cfg::kGainStepDb;
}

// Зберігає пресет у Settings (запис у NVS — з дебаунсом, лише коли значення справді змінились).
void storeEqPreset(Settings& s, void* ctx) {
    memcpy(s.eqGainsDb, ctx, sizeof(s.eqGainsDb));
}

void handleEqGet(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/eq")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    JsonDocument doc;
    fillEqState(doc);
    sendJson(req, 200, doc);
}

void handleEqPost(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/eq")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    if (AppState::snapshot().mode == Mode::OtaUpdate) {
        sendError(req, 409, "ota_in_progress");
        return;
    }
    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    for (JsonPairConst kv : root) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "gainsDb") != 0 && strcmp(k, "band") != 0 && strcmp(k, "gainDb") != 0) {
            sendFieldError(req, 400, "unknown_field", "unknown");
            return;
        }
    }

    int8_t cur[eq_cfg::kBandCount];
    int8_t next[eq_cfg::kBandCount];
    eq::getAllDb(cur);
    memcpy(next, cur, sizeof(next));

    JsonVariantConst vArr = root["gainsDb"];
    JsonVariantConst vBand = root["band"];
    JsonVariantConst vGain = root["gainDb"];

    if (!vArr.isNull()) {
        if (!vBand.isNull() || !vGain.isNull()) {
            sendFieldError(req, 400, "conflicting_fields", "gainsDb");
            return;
        }
        if (!vArr.is<JsonArrayConst>()) {
            sendFieldError(req, 400, "invalid_value", "gainsDb");
            return;
        }
        JsonArrayConst a = vArr.as<JsonArrayConst>();
        if (a.size() != eq_cfg::kBandCount) {
            sendFieldError(req, 400, "invalid_value", "gainsDb");
            return;
        }
        for (uint8_t i = 0; i < eq_cfg::kBandCount; ++i) {
            JsonVariantConst v = a[i];
            if (v.isNull()) continue;  // null = лишити без змін
            if (!v.is<int>()) {
                sendFieldError(req, 400, "invalid_value", "gainsDb");
                return;
            }
            const int x = v.as<int>();
            if (x < eq_cfg::kGainMinDb || x > eq_cfg::kGainMaxDb) {
                sendRangeError(req, "gainsDb", eq_cfg::kGainMinDb, eq_cfg::kGainMaxDb);
                return;
            }
            next[i] = static_cast<int8_t>(x);
        }
    } else if (!vBand.isNull() || !vGain.isNull()) {
        if (vBand.isNull()) {
            sendFieldError(req, 400, "missing_field", "band");
            return;
        }
        if (vGain.isNull()) {
            sendFieldError(req, 400, "missing_field", "gainDb");
            return;
        }
        if (!vBand.is<int>()) {
            sendFieldError(req, 400, "invalid_value", "band");
            return;
        }
        if (!vGain.is<int>()) {
            sendFieldError(req, 400, "invalid_value", "gainDb");
            return;
        }
        const int b = vBand.as<int>();
        if (b < 0 || b >= eq_cfg::kBandCount) {
            sendRangeError(req, "band", 0, eq_cfg::kBandCount - 1);
            return;
        }
        const int x = vGain.as<int>();
        if (x < eq_cfg::kGainMinDb || x > eq_cfg::kGainMaxDb) {
            sendRangeError(req, "gainDb", eq_cfg::kGainMinDb, eq_cfg::kGainMaxDb);
            return;
        }
        next[b] = static_cast<int8_t>(x);
    } else {
        sendFieldError(req, 400, "missing_field", "gainsDb");
        return;
    }

    if (memcmp(cur, next, sizeof(next)) != 0) {
        eq::setAllDb(next);                          // звук змінюється одразу
        SettingsStore::modify(storeEqPreset, next);  // NVS — з дебаунсом
    }
    JsonDocument doc;
    doc["ok"] = true;
    fillEqState(doc);
    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// [Prompt 39] GET /api/inputs, POST /api/inputs — доступність і відключення входів
//   GET : {"inputs":[{"index":0,"name":"WiFi Radio","available":true,"enabled":true,
//                     "selectable":true,"canDisable":false}, ... 4 входи ...],
//          "enabledMask":15,"input":0}
//         available  = вхід апаратно є (processor.capabilities.inputCount; PT2313L не має входу 3);
//         enabled    = дозволено користувачем (маска); selectable = available && enabled;
//         canDisable = false лише для радіо (вхід 0).
//   POST: {"enabled":[a,b,c,d]}      рівно 4 елементи: true | false | null (= не змінювати);
//                                    елемент 0 (радіо) ігнорується: радіо завжди дозволене
//         або {"index":N,"enabled":true|false}   (одне поле, N = 0..3)
//         -> 200 {"ok":true, ...те саме, що GET}; 409 ota_in_progress | ir_learn_active; 503 busy.
// Дозволено і в Standby. Зберігається в NVS (з дебаунсом, лише при зміні) і діє після перезапуску.
// ---------------------------------------------------------------------------
// ВАЖЛИВО: імена в doc (char-масиви Settings) ArduinoJson може лише ПОСИЛАТИ, а не копіювати, тож
// Settings `st` мусить жити в викликача, поки doc не серіалізовано (sendJson() — синхронний).
void fillInputsState(JsonDocument& doc, const Settings& st) {
    const uint8_t mask = settings_cfg::normalizeInputMask(st.inputEnabledMask);
    uint8_t hw = defaults::kInputCount;  // апаратно доступні входи (без процесора — усі 4, як у контролері)
    if (s_proc != nullptr) {
        const uint8_t n = s_proc->capabilities().inputCount;
        if (n < hw) hw = n;
    }
    JsonArray arr = doc["inputs"].to<JsonArray>();
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        const bool available = (i < hw);
        const bool enabled = ((mask >> i) & 1u) != 0;
        JsonObject o = arr.add<JsonObject>();
        o["index"] = i;
        o["name"] = (st.inputNames[i][0] != '\0') ? st.inputNames[i] : defaults::kInputNames[i];
        o["available"] = available;
        o["enabled"] = enabled;
        o["selectable"] = available && enabled;
        o["canDisable"] = (i != 0);
    }
    doc["enabledMask"] = mask;
    doc["input"] = AppState::snapshot().inputIndex;  // активний вхід (міг змінитись після вимкнення)
}

void handleInputsGet(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/inputs")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());
    const Settings st = SettingsStore::snapshot();
    JsonDocument doc;
    fillInputsState(doc, st);
    sendJson(req, 200, doc);
}

void handleInputsPost(AsyncWebServerRequest* req) {
    if (!exactUrl(req, "/api/inputs")) return;
    WEB_API_LOG("%s %s", req->methodToString(), req->url().c_str());

    JsonDocument in;
    if (!readJsonObjectBody(req, in)) return;
    JsonObjectConst root = in.as<JsonObjectConst>();
    if (!allowKeys(req, root, "enabled", "index")) return;

    JsonVariantConst vEn = root["enabled"];
    JsonVariantConst vIdx = root["index"];
    if (vEn.isNull()) {
        sendFieldError(req, 400, "missing_field", "enabled");
        return;
    }

    const uint8_t cur =
        settings_cfg::normalizeInputMask(SettingsStore::snapshot().inputEnabledMask);
    uint8_t next = cur;

    if (vEn.is<JsonArrayConst>()) {
        if (!vIdx.isNull()) {
            sendFieldError(req, 400, "conflicting_fields", "enabled");
            return;
        }
        JsonArrayConst a = vEn.as<JsonArrayConst>();
        if (a.size() != defaults::kInputCount) {
            sendFieldError(req, 400, "invalid_value", "enabled");
            return;
        }
        for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
            JsonVariantConst v = a[i];
            if (v.isNull()) continue;  // null = лишити без змін
            if (!v.is<bool>()) {
                sendFieldError(req, 400, "invalid_value", "enabled");
                return;
            }
            if (v.as<bool>()) {
                next = static_cast<uint8_t>(next | (1u << i));
            } else {
                next = static_cast<uint8_t>(next & ~(1u << i));
            }
        }
    } else if (vEn.is<bool>()) {
        if (vIdx.isNull()) {
            sendFieldError(req, 400, "missing_field", "index");
            return;
        }
        if (!vIdx.is<int>()) {
            sendFieldError(req, 400, "invalid_value", "index");
            return;
        }
        const int idx = vIdx.as<int>();
        if (idx < 0 || idx >= defaults::kInputCount) {
            sendRangeError(req, "index", 0, defaults::kInputCount - 1);
            return;
        }
        if (vEn.as<bool>()) {
            next = static_cast<uint8_t>(next | (1u << idx));
        } else {
            next = static_cast<uint8_t>(next & ~(1u << idx));
        }
    } else {
        sendFieldError(req, 400, "invalid_value", "enabled");
        return;
    }
    next = settings_cfg::normalizeInputMask(next);  // радіо (біт 0) лишається дозволеним

    // Завжди через контролер (навіть без змін): ті самі гейти (OTA, IR) і одна точка запису.
    const char* why = nullptr;
    const WebCmdResult r = AppController::runWebCommand(
        {WebCmdType::InputsEnabledSet, static_cast<int32_t>(next), false}, &why);
    WEB_API_LOG("inputs mask 0x%02X -> result %d (%s)", static_cast<unsigned>(next),
                static_cast<int>(r), why != nullptr ? why : "-");
    if (r != WebCmdResult::Ok) {
        sendResult(req, r, why);
        return;
    }
    const Settings st = SettingsStore::snapshot();  // контролер уже записав маску синхронно
    JsonDocument doc;
    doc["ok"] = true;
    fillInputsState(doc, st);
    sendJson(req, 200, doc);
}

}  // namespace

void registerPlayerRoutes(AsyncWebServer& server, AudioProcessor* proc) {
    s_proc = proc;

    // ПОРЯДОК ВАЖЛИВИЙ (як у web_server.cpp): "/api/player/station" спершу, потім
    // "/api/player/*"; обробники точного шляху ще й перевіряють url самі.
    server.on("/api/player/station", HTTP_POST, handlePlayerStation, nullptr,
              web_api::jsonBodyCallback);
    server.on("/api/player/*", HTTP_POST, handlePlayerAction);
    server.on("/api/power", HTTP_POST, handlePower, nullptr, web_api::jsonBodyCallback);
    server.on("/api/mute", HTTP_POST, handleMute, nullptr, web_api::jsonBodyCallback);
    server.on("/api/volume", HTTP_POST, handleVolume, nullptr, web_api::jsonBodyCallback);
    server.on("/api/gain", HTTP_POST, handleGain, nullptr, web_api::jsonBodyCallback);
    server.on("/api/input", HTTP_POST, handleInput, nullptr, web_api::jsonBodyCallback);

    // [Prompt 30] Еквалайзер радіо.
    server.on("/api/eq", HTTP_GET, handleEqGet);
    server.on("/api/eq", HTTP_POST, handleEqPost, nullptr, web_api::jsonBodyCallback);

    // [Prompt 39] Доступність і відключення входів. "/api/inputs" не плутається з "/api/input":
    // ESPAsyncWebServer зіставляє "/x" лише з "/x" і "/x/...", а не з "/xs".
    server.on("/api/inputs", HTTP_GET, handleInputsGet);
    server.on("/api/inputs", HTTP_POST, handleInputsPost, nullptr, web_api::jsonBodyCallback);
}
