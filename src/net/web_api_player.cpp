// net/web_api_player.cpp (Prompt 18): POST /api/power, /api/mute, /api/volume, /api/gain,
// /api/input, /api/player/*, /api/player/station.
//
// Усе, що міняє стан пристрою, виконує AppController::runWebCommand() під його мʼютексом
// (те саме, що кнопки/пульт: ramp, мʼют на переходах, збереження в Settings). Тут лише:
// розбір і валідація тіла, перевірка меж за capabilities(), мапінг WebCmdResult -> HTTP-код.
//
// Коди: 200 ok | 400 помилка клієнта (інвалідне значення/поза межами/не підтримується) |
//       404 невідомий підшлях | 409 зараз недопустимо (standby, OTA, навчання IR, не вхід
//       Radio, немає станцій) | 503 контролер зайнятий або немає аудіопроцесора.

#include "net/web_api_player.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "audio/audio_processor.h"
#include "core/app_controller.h"
#include "core/app_state.h"
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
}
