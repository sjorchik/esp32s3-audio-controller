// WifiManager (Prompt 12): STA -> (3 невдачі | немає мережі | скидання) -> AP + captive portal.
//
// Усі виклики WiFi.* робить ОДНА внутрішня задача. HTTP-обробники порталу й веб-API
// (net/web_api_wifi.cpp) лише читають кеш скану й залишають запити через прапорці під
// мʼютексом (шаблон «прапорець + задача»).
//
// [Prompt 28] stop(): запит на повне вимкнення Wi-Fi (офлайн-режим). Виконує задача:
// DNS стоп, HTTP-портал end(), softAP/STA вниз, WiFi.mode(WIFI_OFF), Phase::Off.
//
// [Prompt 29] Кілька збережених мереж. Список (SSID + пароль, порядок = пріоритет) тримає
// net/wifi_networks (NVS "wifinets"); тут — вибір мережі серед них:
//   Раунд = [скан, якщо збережено >= 2 мереж] -> кандидати = збережені мережі, ВИДИМІ в скані,
//   за пріоритетом (жодної не видно, напр. приховані SSID, — усі збережені підряд напряму)
//   -> спроби по черзі (WiFi.begin(ssid, pass) із власним тайм-аутом).
//   FirstConnect (старт): kWifiRetryBeforeAp невдалих раундів або вичерпаний бюджет
//   kApBudgetMs -> AP (як і раніше). Reconnect (втрата звʼязку під час роботи): раунди
//   повторюються через kReconnectPeriodMs без переходу в AP. Поки пристрій підключений, на
//   мережу з вищим пріоритетом сам НЕ перемикається.
//   Одна збережена мережа: без скану, пряма спроба — поведінка як до P29.
// Скан для API (POST /api/wifi/scan) виконується тут же в Monitor; примусове підключення
// (POST /api/wifi/connect) — handleConnectRequest().
// Стару одиничну мережу з esp_wifi (P12) переносить у список migrateLegacyNetwork().

#include "net/wifi_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "config/defaults.h"
#include "config/wifi_config.h"
#include "net/mdns.h"
#include "net/wifi_networks.h"

#if WIFI_MANAGER_DEBUG
#define WIFI_LOG(...) Serial.printf("[WIFI] " __VA_ARGS__)
#else
#define WIFI_LOG(...) \
    do {              \
    } while (0)
#endif

namespace {

static_assert(sizeof(WifiScanEntry::ssid) == wifi_cfg::kSsidMax + 1,
              "WifiScanEntry::ssid must match wifi_cfg::kSsidMax + 1");

// ---------------------------------------------------------------------------
// Сторінка порталу. Тримається в прошивці (а не в LittleFS): портал має працювати
// навіть із порожнім/пошкодженим LittleFS і не залежить від веб-сторінок Prompt 13.
// SSID вставляються через textContent (без innerHTML), щоб назва мережі не
// могла виконати скрипт.
// ---------------------------------------------------------------------------
const char kPortalHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Audio Controller Wi-Fi</title>
<style>
body{font-family:sans-serif;margin:0;padding:16px;background:#111;color:#eee;max-width:480px}
h1{font-size:20px;margin:0 0 12px}
button,input{font-size:16px;padding:10px;border-radius:6px;border:1px solid #555;background:#222;color:#eee;width:100%;box-sizing:border-box;margin:4px 0}
button.net{text-align:left}
button.go{background:#0a5;border-color:#0a5}
#msg{min-height:20px;color:#fb3}
small{color:#999}
</style></head><body>
<h1>Wi-Fi setup</h1>
<div id="list">Scanning...</div>
<button id="rescan" type="button">Rescan</button>
<input id="ssid" placeholder="Network name (SSID)" maxlength="32" autocapitalize="off" autocorrect="off">
<input id="pass" type="password" placeholder="Password (empty for open network)" maxlength="63">
<button id="go" class="go" type="button">Connect</button>
<p id="msg"></p>
<small>After a successful connection the device restarts and joins your network.
If this network reappears, the password was wrong - try again.</small>
<script>
var $=function(i){return document.getElementById(i)};
function render(nets){
  var l=$('list');l.textContent='';
  if(!nets.length){l.textContent='No networks found';return}
  nets.forEach(function(n){
    var b=document.createElement('button');b.type='button';b.className='net';
    b.textContent=n.ssid+(n.secure?' (secured)':'')+'  '+n.rssi+' dBm';
    b.onclick=function(){$('ssid').value=n.ssid;$('pass').focus()};
    l.appendChild(b);
  });
}
function load(){
  fetch('/scan').then(function(r){return r.json()}).then(function(d){
    if(d.scanning){$('list').textContent='Scanning...';setTimeout(load,1500);return}
    render(d.networks);
  }).catch(function(){setTimeout(load,3000)});
}
$('rescan').onclick=function(){
  $('list').textContent='Scanning...';
  fetch('/scan',{method:'POST'}).then(function(){setTimeout(load,1500)});
};
$('go').onclick=function(){
  var s=$('ssid').value,p=$('pass').value;
  if(!s){$('msg').textContent='Enter or pick a network';return}
  $('msg').textContent='Connecting... the device will restart on success.';
  fetch('/connect',{method:'POST',body:new URLSearchParams({ssid:s,pass:p})})
    .then(function(r){return r.json()})
    .then(function(d){if(!d.ok)$('msg').textContent=d.error||'Rejected'})
    .catch(function(){});
};
fetch('/status').then(function(r){return r.json()}).then(function(d){
  if(d.conn==='failed')$('msg').textContent='Last attempt failed: check the password.';
}).catch(function(){});
load();
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// Стан
// ---------------------------------------------------------------------------
enum class Phase : uint8_t {
    FirstConnect,  // перші спроби після старту (рахуються до AP)
    Monitor,       // підключено, стежимо за втратою звʼязку
    Reconnect,     // звʼязок втрачено під час роботи (або примусове підключення): без переходу в AP
    Ap,            // AP + captive portal
    Off,           // [Prompt 28] Wi-Fi вимкнено (офлайн): задача лише спить
};

// [Prompt 29] Крок вибору мережі (FirstConnect / Reconnect).
enum class Step : uint8_t {
    Scan,   // чекаємо на скан (власний або чужий, що вже йшов)
    Try,    // спроба підключення до кандидата
    Pause,  // пауза між раундами
};

enum class PortalConn : uint8_t { Idle, Trying, Failed };

// [Prompt 29] Результат останнього завершеного скану.
enum class ScanResult : uint8_t { None, Done, Failed };

struct Net {
    char ssid[wifi_cfg::kSsidMax + 1];
    int8_t rssi;
    uint8_t channel;  // [Prompt 29]
    bool secure;
};

SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
bool s_forceReset = false;
volatile bool s_stopRequested = false;  // [Prompt 28] ставить stop(), знімає задача

// --- Спільне (захищене s_lock; s_state/s_rssi/s_channel/s_phase читаються без нього) ---
volatile WifiState s_state = WifiState::Connecting;
volatile int32_t s_rssi = 0;
volatile int32_t s_channel = 0;  // [Prompt 29]
volatile Phase s_phase = Phase::FirstConnect;  // [Prompt 29] volatile: читає requestScan/requestConnect
char s_ssid[wifi_cfg::kSsidMax + 1] = "";
char s_ip[16] = "";

Net s_nets[wifi_cfg::kMaxNetworks];
uint8_t s_netCount = 0;
bool s_scanning = false;
bool s_scanRequested = false;
volatile ScanResult s_scanResult = ScanResult::None;  // [Prompt 29]
uint32_t s_scanDoneMs = 0;                            // [Prompt 29]

bool s_connectRequested = false;
char s_reqSsid[wifi_cfg::kSsidMax + 1] = "";
char s_reqPass[wifi_cfg::kPassMax + 1] = "";
PortalConn s_portalConn = PortalConn::Idle;

// [Prompt 29] Запит примусового підключення з API (SSID, а не індекс: список міг змінитись).
bool s_apiConnectPending = false;
char s_apiConnectSsid[wifi_cfg::kSsidMax + 1] = "";

// --- Лише задача WifiManager ---
uint32_t s_lastRssiMs = 0;
uint32_t s_lastApCheckMs = 0;
uint32_t s_scanStartMs = 0;
bool s_scanOwnerEngine = false;  // [Prompt 29] поточний скан запустив вибір мережі
char s_apIpStr[16] = "";
char s_portalUrl[32] = "";

// [Prompt 29] Вибір мережі. Знімок списку (З ПАРОЛЯМИ) живе лише на час раунду й затирається.
WifiSavedNet s_snap[wifi_cfg::kMaxSavedNetworks];
uint8_t s_snapCount = 0;
bool s_seen[wifi_cfg::kMaxSavedNetworks];  // збережена мережа видима в останньому скані
uint8_t s_cand[wifi_cfg::kMaxCandidatesPerRound];
uint8_t s_candCount = 0;
uint8_t s_candPos = 0;
Step s_step = Step::Try;
bool s_scanStarted = false;
uint32_t s_attemptMs = 0;
uint32_t s_stepMs = 0;
uint32_t s_selectStartMs = 0;
uint8_t s_rounds = 0;
char s_forceSsid[wifi_cfg::kSsidMax + 1] = "";
uint32_t s_lastPersistTryMs = 0;

DNSServer s_dns;
AsyncWebServer* s_server = nullptr;

class Lock {
public:
    Lock()
        : m_ok(s_lock != nullptr &&
               xSemaphoreTake(s_lock, pdMS_TO_TICKS(wifi_cfg::kLockTimeoutMs)) == pdTRUE) {}
    ~Lock() {
        if (m_ok) xSemaphoreGive(s_lock);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    bool ok() const { return m_ok; }

private:
    bool m_ok;
};

inline bool elapsed(uint32_t now, uint32_t since, uint32_t ms) {
    return static_cast<uint32_t>(now - since) >= ms;
}

// Затирання, яке компілятор не прибере (паролі).
void secureZero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n-- > 0) *v++ = 0;
}

const char* stateName(WifiState s) {
    switch (s) {
        case WifiState::Connecting:        return "Connecting";
        case WifiState::Connected:         return "Connected";
        case WifiState::ApMode:            return "ApMode";
        case WifiState::ApClientConnected: return "ApClientConnected";
        case WifiState::Off:               return "Off";  // [Prompt 28]
    }
    return "?";
}

void setInfo(WifiState st, const char* ssid, const char* ip) {
    {
        Lock l;
        if (l.ok()) {
            memset(s_ssid, 0, sizeof(s_ssid));
            memset(s_ip, 0, sizeof(s_ip));
            strlcpy(s_ssid, ssid, sizeof(s_ssid));
            strlcpy(s_ip, ip, sizeof(s_ip));
        }
    }
    if (st != s_state) {
        WIFI_LOG("state %s -> %s\n", stateName(s_state), stateName(st));
    }
    s_state = st;
}

// Збережена в NVS мережа STA (esp_wifi). НЕ WiFi.SSID(): у Arduino-ESP32 3.x воно повертає
// SSID поточного підключення (порожній рядок, поки зʼєднання немає).
// Викликати після WiFi.mode(WIFI_STA) (esp_wifi вже ініціалізовано).
bool savedSsid(char* out, size_t cap) {
    out[0] = '\0';
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    const bool ok = (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != '\0');
    if (ok) {
        const size_t n = strnlen(reinterpret_cast<const char*>(conf.sta.ssid), sizeof(conf.sta.ssid));
        const size_t m = (n < cap - 1) ? n : cap - 1;
        memcpy(out, conf.sta.ssid, m);
        out[m] = '\0';
    }
    secureZero(&conf, sizeof(conf));
    return ok;
}

const char* statusName(wl_status_t st) {
    switch (st) {
        case WL_IDLE_STATUS:     return "idle";
        case WL_NO_SSID_AVAIL:   return "no ssid";
        case WL_SCAN_COMPLETED:  return "scan done";
        case WL_CONNECTED:       return "connected";
        case WL_CONNECT_FAILED:  return "connect failed";
        case WL_CONNECTION_LOST: return "connection lost";
        case WL_DISCONNECTED:    return "disconnected";
        default:                 return "?";
    }
}

// ---------------------------------------------------------------------------
// HTTP-обробники порталу (виконуються в задачі AsyncTCP: коротко, без WiFi.*)
// ---------------------------------------------------------------------------
void handleRoot(AsyncWebServerRequest* r) {
    r->send(200, "text/html", kPortalHtml);
}

void handleScanGet(AsyncWebServerRequest* r) {
    JsonDocument doc;
    String out;
    {
        Lock l;
        doc["scanning"] = l.ok() ? s_scanning : true;
        JsonArray arr = doc["networks"].to<JsonArray>();
        if (l.ok()) {
            for (uint8_t i = 0; i < s_netCount; ++i) {
                JsonObject o = arr.add<JsonObject>();
                o["ssid"] = s_nets[i].ssid;
                o["rssi"] = s_nets[i].rssi;
                o["secure"] = s_nets[i].secure;
            }
        }
        // Серіалізація під мʼютексом: s_nets не змінюється, поки JSON збирається.
        serializeJson(doc, out);
    }
    r->send(200, "application/json", out);
}

void handleScanPost(AsyncWebServerRequest* r) {
    {
        Lock l;
        if (l.ok()) s_scanRequested = true;
    }
    r->send(200, "application/json", "{}");
}

void handleStatus(AsyncWebServerRequest* r) {
    const char* conn = "idle";
    {
        Lock l;
        if (l.ok()) {
            conn = (s_portalConn == PortalConn::Trying)  ? "trying"
                   : (s_portalConn == PortalConn::Failed) ? "failed"
                                                          : "idle";
        }
    }
    String out = String("{\"conn\":\"") + conn + "\"}";
    r->send(200, "application/json", out);
}

void handleConnect(AsyncWebServerRequest* r) {
    if (!r->hasParam("ssid", true)) {
        r->send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
        return;
    }
    const String ssid = r->getParam("ssid", true)->value();
    const String pass = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : String();

    // [Prompt 29] Та сама перевірка, що й у списку мереж (довжини як раніше + керівні символи):
    // інакше успішно підключена мережа не потрапила б у список.
    if (WifiNetworks::validate(ssid.c_str(), pass.c_str()) != WifiNetResult::Ok) {
        r->send(400, "application/json",
                "{\"ok\":false,\"error\":\"bad ssid or password (8..63 chars)\"}");
        return;
    }

    bool accepted = false;
    {
        Lock l;
        if (l.ok() && !s_connectRequested && s_portalConn != PortalConn::Trying) {
            strlcpy(s_reqSsid, ssid.c_str(), sizeof(s_reqSsid));
            strlcpy(s_reqPass, pass.c_str(), sizeof(s_reqPass));
            s_connectRequested = true;
            s_portalConn = PortalConn::Trying;
            accepted = true;
        }
    }
    if (accepted) {
        r->send(200, "application/json", "{\"ok\":true}");
    } else {
        r->send(409, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    }
}

// Усі невідомі шляхи (перевірки captive portal Android/iOS/Windows) -> на портал.
void handleNotFound(AsyncWebServerRequest* r) {
    r->redirect(s_portalUrl);
}

void startPortalServer() {
    if (s_server != nullptr) {
        return;
    }
    s_server = new AsyncWebServer(wifi_cfg::kPortalHttpPort);
    s_server->on("/", HTTP_GET, handleRoot);
    s_server->on("/scan", HTTP_GET, handleScanGet);
    s_server->on("/scan", HTTP_POST, handleScanPost);
    s_server->on("/status", HTTP_GET, handleStatus);
    s_server->on("/connect", HTTP_POST, handleConnect);
    s_server->onNotFound(handleNotFound);
    s_server->begin();
}

// ---------------------------------------------------------------------------
// Скан мереж (асинхронний, ініціюється й опитується з задачі)
// ---------------------------------------------------------------------------
// forEngine = true: скан для вибору мережі — pollScan() позначає видимі збережені мережі
// (s_seen відносно знімка s_snap). chMs — час на канал (STA: коротший, щоб менше переривати потік).
void startScan(uint32_t chMs, bool forEngine) {
    {
        Lock l;
        s_scanRequested = false;
        s_scanning = true;
    }
    s_scanOwnerEngine = forEngine;
    if (forEngine) {
        memset(s_seen, 0, sizeof(s_seen));
    }
    WiFi.scanDelete();
    const int r = WiFi.scanNetworks(true, false, false, chMs);
    s_scanStartMs = millis();
    if (r == WIFI_SCAN_FAILED) {
        Lock l;
        s_scanning = false;
        s_scanResult = ScanResult::Failed;
        WIFI_LOG("scan start FAILED\n");
        return;
    }
    WIFI_LOG("scan started%s\n", forEngine ? " (network selection)" : "");
}

void pollScan() {
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        if (elapsed(millis(), s_scanStartMs, wifi_cfg::kScanTimeoutMs)) {
            WiFi.scanDelete();
            Lock l;
            s_scanning = false;
            s_scanResult = ScanResult::Failed;
            WIFI_LOG("scan timeout\n");
        }
        return;
    }
    if (n < 0) {
        Lock l;
        s_scanning = false;
        s_scanResult = ScanResult::Failed;
        WIFI_LOG("scan failed (%d)\n", n);
        return;
    }

    Net tmp[wifi_cfg::kMaxNetworks];
    uint8_t cnt = 0;
    for (int i = 0; i < n; ++i) {
        const String name = WiFi.SSID(i);
        if (name.length() == 0) {
            continue;  // прихована мережа: вводиться вручну
        }
        const int8_t rssi = static_cast<int8_t>(WiFi.RSSI(i));
        const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        const uint8_t channel = static_cast<uint8_t>(WiFi.channel(i));

        // [Prompt 29] Видимість збережених мереж визначаємо за ВСІМА результатами (список для
        // показу нижче обмежений kMaxNetworks і міг би витіснити слабку збережену мережу).
        if (s_scanOwnerEngine) {
            for (uint8_t j = 0; j < s_snapCount; ++j) {
                if (strcmp(s_snap[j].ssid, name.c_str()) == 0) s_seen[j] = true;
            }
        }

        int found = -1;
        for (uint8_t j = 0; j < cnt; ++j) {
            if (strcmp(tmp[j].ssid, name.c_str()) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {  // кілька точок з одним SSID: лишаємо найсильнішу
            if (rssi > tmp[found].rssi) {
                tmp[found].rssi = rssi;
                tmp[found].secure = secure;
                tmp[found].channel = channel;
            }
            continue;
        }
        if (cnt < wifi_cfg::kMaxNetworks) {
            strlcpy(tmp[cnt].ssid, name.c_str(), sizeof(tmp[cnt].ssid));
            tmp[cnt].rssi = rssi;
            tmp[cnt].secure = secure;
            tmp[cnt].channel = channel;
            ++cnt;
        } else {  // повно: витісняємо найслабшу, якщо ця сильніша
            uint8_t w = 0;
            for (uint8_t j = 1; j < cnt; ++j) {
                if (tmp[j].rssi < tmp[w].rssi) w = j;
            }
            if (rssi > tmp[w].rssi) {
                strlcpy(tmp[w].ssid, name.c_str(), sizeof(tmp[w].ssid));
                tmp[w].rssi = rssi;
                tmp[w].secure = secure;
                tmp[w].channel = channel;
            }
        }
    }
    WiFi.scanDelete();

    // Сортування за спаданням RSSI (вставками; cnt <= kMaxNetworks).
    for (uint8_t i = 1; i < cnt; ++i) {
        Net key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].rssi < key.rssi) {
            tmp[j + 1] = tmp[j];
            --j;
        }
        tmp[j + 1] = key;
    }

    Lock l;
    if (l.ok()) {
        memcpy(s_nets, tmp, sizeof(Net) * cnt);
        s_netCount = cnt;
    }
    s_scanning = false;
    s_scanResult = ScanResult::Done;
    s_scanDoneMs = millis();
    WIFI_LOG("scan done: %u networks\n", static_cast<unsigned>(cnt));
}

bool scanningNow() {
    Lock l;
    return l.ok() ? s_scanning : true;
}

// Скасувати скан, що триває (перед примусовим підключенням). Лише з задачі.
void abortScan() {
    bool sc = false;
    {
        Lock l;
        if (l.ok()) {
            sc = s_scanning;
            s_scanRequested = false;
        }
    }
    if (!sc) return;
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    Lock l;
    s_scanning = false;
    s_scanResult = ScanResult::None;
    WIFI_LOG("scan aborted\n");
}

// ---------------------------------------------------------------------------
// STA
// ---------------------------------------------------------------------------
void wipeSnapshot() {
    secureZero(s_snap, sizeof(s_snap));
    s_snapCount = 0;
    s_candCount = 0;
    s_candPos = 0;
}

void onConnected() {
    const String ssid = WiFi.SSID();
    const String ip = WiFi.localIP().toString();
    s_rssi = WiFi.RSSI();
    s_channel = WiFi.channel();
    s_lastRssiMs = millis();
    setInfo(WifiState::Connected, ssid.c_str(), ip.c_str());
    WIFI_LOG("STA connected: \"%s\" ip=%s rssi=%d ch=%d\n", ssid.c_str(), ip.c_str(),
             static_cast<int>(s_rssi), static_cast<int>(s_channel));
    WIFI_LOG("task stack free (min): %u bytes\n",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    MdnsManager::begin();  // ідемпотентно: реально стартує лише один раз
}

void resetCredentials() {
    WiFi.disconnect(false, true);  // STA вже запущено; true = стерти збережені дані esp_wifi
    char left[wifi_cfg::kSsidMax + 1];
    if (!savedSsid(left, sizeof(left))) {
        WIFI_LOG("saved network erased\n");
    } else {
        WIFI_LOG("WARNING: saved network still present after erase: \"%s\"\n", left);
    }
}

// [Prompt 29] Перенос одиничної мережі зі старого формату (P12: облікові дані лише в esp_wifi)
// у список: стає записом №1. Нічого не стирає: перше вмикання після оновлення прошивки
// підключається само. Викликається, лише якщо списку в NVS ще не було.
void migrateLegacyNetwork() {
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || conf.sta.ssid[0] == '\0') {
        WIFI_LOG("migration: no legacy network\n");
        secureZero(&conf, sizeof(conf));
        return;
    }
    char ssid[sizeof(conf.sta.ssid) + 1];
    char pass[sizeof(conf.sta.password) + 1];
    const size_t sl = strnlen(reinterpret_cast<const char*>(conf.sta.ssid), sizeof(conf.sta.ssid));
    const size_t pl =
        strnlen(reinterpret_cast<const char*>(conf.sta.password), sizeof(conf.sta.password));
    memcpy(ssid, conf.sta.ssid, sl);
    ssid[sl] = '\0';
    memcpy(pass, conf.sta.password, pl);
    pass[pl] = '\0';
    secureZero(&conf, sizeof(conf));

    uint8_t idx = 0;
    const WifiNetResult r = WifiNetworks::add(ssid, pass, 0, false, &idx, nullptr);
    secureZero(pass, sizeof(pass));
    if (r != WifiNetResult::Ok) {
        WIFI_LOG("migration skipped for \"%s\" (code %u): enter the network again\n", ssid,
                 static_cast<unsigned>(r));
        return;
    }
    for (uint8_t i = 0; i < 3 && !WifiNetworks::persistIfDirty(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    WIFI_LOG("migration: legacy network \"%s\" -> list #%u\n", ssid, static_cast<unsigned>(idx + 1));
}

// Запис списку в NVS — у задачі wifi (HTTP-обробники лише позначають список «брудним»).
void persistTick() {
    if (!WifiNetworks::isDirty()) return;
    const uint32_t now = millis();
    if (!elapsed(now, s_lastPersistTryMs, wifi_cfg::kPersistRetryMs)) return;
    s_lastPersistTryMs = now;
    if (!WifiNetworks::persistIfDirty()) {
        WIFI_LOG("network list not saved, will retry\n");
    }
}

// ---------------------------------------------------------------------------
// AP + captive portal
// ---------------------------------------------------------------------------
void enterAp() {
    wipeSnapshot();
    {
        Lock l;
        if (l.ok()) s_apiConnectPending = false;
    }
    WiFi.disconnect(false, false);  // зупинити спроби STA
    WiFi.mode(WIFI_AP_STA);         // STA потрібна для сканування мереж

    const IPAddress ip(wifi_cfg::kApIp[0], wifi_cfg::kApIp[1], wifi_cfg::kApIp[2],
                       wifi_cfg::kApIp[3]);
    const IPAddress mask(wifi_cfg::kApNetmask[0], wifi_cfg::kApNetmask[1],
                         wifi_cfg::kApNetmask[2], wifi_cfg::kApNetmask[3]);
    WiFi.softAPConfig(ip, ip, mask);
    const char* pass = (wifi_cfg::kApPassword[0] != '\0') ? wifi_cfg::kApPassword : nullptr;
    if (!WiFi.softAP(defaults::kApSsid, pass, wifi_cfg::kApChannel, 0, wifi_cfg::kApMaxClients)) {
        WIFI_LOG("softAP start FAILED\n");
    }

    const String ipStr = ip.toString();
    strlcpy(s_apIpStr, ipStr.c_str(), sizeof(s_apIpStr));
    snprintf(s_portalUrl, sizeof(s_portalUrl), "http://%s/", s_apIpStr);

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(wifi_cfg::kDnsPort, "*", ip);  // усі імена -> адреса AP
    startPortalServer();

    s_rssi = 0;
    s_channel = 0;
    setInfo(WifiState::ApMode, defaults::kApSsid, s_apIpStr);
    s_phase = Phase::Ap;
    s_lastApCheckMs = millis();
    {
        Lock l;
        s_scanRequested = true;  // перший скан — до приходу клієнтів
    }
    WIFI_LOG("AP \"%s\" up, portal %s\n", defaults::kApSsid, s_portalUrl);
    WIFI_LOG("task stack free (min): %u bytes\n",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
}

// [Prompt 28] Повне вимкнення Wi-Fi (викликає лише задача WifiManager). Порядок: спершу
// скасовуємо запити порталу й зупиняємо DNS/HTTP, потім радіо. Збережену STA-мережу не
// стираємо (disconnect(false, false)): наступний запуск проходить звичайний сценарій.
void shutdownRadio() {
    WIFI_LOG("stop: shutting down portal, AP and Wi-Fi radio\n");
    {
        Lock l;
        if (l.ok()) {
            s_scanRequested = false;
            s_scanning = false;
            s_connectRequested = false;
            s_apiConnectPending = false;  // [Prompt 29]
            s_portalConn = PortalConn::Idle;
            memset(s_reqPass, 0, sizeof(s_reqPass));
        }
    }
    wipeSnapshot();  // [Prompt 29]
    s_dns.stop();
    if (s_server != nullptr) {
        s_server->end();  // об'єкт лишаємо (його могли б ще використовувати обробники AsyncTCP)
    }
    WiFi.scanDelete();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    s_rssi = 0;
    s_channel = 0;
    setInfo(WifiState::Off, "", "");
    s_phase = Phase::Off;
    s_stopRequested = false;
    WIFI_LOG("stop: Wi-Fi is off\n");
}

// Підключення з порталу: WiFi.begin(ssid, pass) (бібліотека сама збереже дані) ->
// очікування -> [P29: мережа додається в СПИСОК першою] -> перезапуск. Чистий перехід у STA на
// наступному завантаженні простіший за перемикання режиму на льоту (AP, DNS і HTTP-сервер
// лишились би живі).
void portalConnect() {
    char ssid[wifi_cfg::kSsidMax + 1];
    char pass[wifi_cfg::kPassMax + 1];
    {
        Lock l;
        if (!l.ok()) return;  // повторимо на наступній ітерації
        strlcpy(ssid, s_reqSsid, sizeof(ssid));
        strlcpy(pass, s_reqPass, sizeof(pass));
        memset(s_reqPass, 0, sizeof(s_reqPass));
        s_connectRequested = false;
    }
    WIFI_LOG("portal: trying \"%s\"\n", ssid);

    vTaskDelay(pdMS_TO_TICKS(wifi_cfg::kPortalReplyFlushMs));  // відповідь має дійти до телефону
    WiFi.begin(ssid, pass[0] != '\0' ? pass : nullptr);

    const uint32_t start = millis();
    wl_status_t st;
    do {
        vTaskDelay(pdMS_TO_TICKS(100));
        st = WiFi.status();
    } while (st != WL_CONNECTED && st != WL_CONNECT_FAILED &&
             !elapsed(millis(), start, wifi_cfg::kPortalConnectTimeoutMs));

    if (st == WL_CONNECTED) {
        // [Prompt 29] Нова мережа — ПЕРШОЮ (пріоритет), запис із тим самим SSID замінюється,
        // решта списку лишається; якщо список повний — випадає остання (найнижчий пріоритет).
        uint8_t idx = 0;
        const WifiNetResult r = WifiNetworks::add(ssid, pass, 0, true, &idx, nullptr);
        secureZero(pass, sizeof(pass));
        bool saved = (r == WifiNetResult::Ok);
        for (uint8_t i = 0; saved && i < 3; ++i) {
            if (WifiNetworks::persistIfDirty()) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            if (i == 2) saved = false;
        }
        char chk[wifi_cfg::kSsidMax + 1];
        WIFI_LOG("portal: connected, list=%s, esp_wifi=%s; restarting\n", saved ? "saved" : "NOT saved",
                 savedSsid(chk, sizeof(chk)) ? "saved" : "NO");
        vTaskDelay(pdMS_TO_TICKS(wifi_cfg::kRestartDelayMs));
        ESP.restart();
        return;
    }
    secureZero(pass, sizeof(pass));

    WIFI_LOG("portal: failed (%s), staying in AP\n", statusName(st));
    WiFi.disconnect(false, true);  // не лишати в esp_wifi мережу, яка не підключилась
    Lock l;
    s_portalConn = PortalConn::Failed;
}

void apTick() {
    s_dns.processNextRequest();

    const uint32_t now = millis();
    if (elapsed(now, s_lastApCheckMs, wifi_cfg::kStaPollMs)) {
        s_lastApCheckMs = now;
        const WifiState want = (WiFi.softAPgetStationNum() > 0) ? WifiState::ApClientConnected
                                                                : WifiState::ApMode;
        if (want != s_state) {
            setInfo(want, defaults::kApSsid, s_apIpStr);
        }
    }

    bool scanReq = false;
    bool scanning = false;
    bool connReq = false;
    {
        Lock l;
        if (l.ok()) {
            scanReq = s_scanRequested;
            scanning = s_scanning;
            connReq = s_connectRequested;
        }
    }

    if (connReq) {
        portalConnect();
    } else if (scanning) {
        pollScan();
    } else if (scanReq) {
        startScan(wifi_cfg::kScanChannelApMs, false);
    }
}

// ---------------------------------------------------------------------------
// [Prompt 29] Вибір мережі серед збережених (FirstConnect / Reconnect)
// ---------------------------------------------------------------------------
void startRound();

// Спроба підключення до s_cand[s_candPos].
void beginAttempt() {
    const WifiSavedNet& n = s_snap[s_cand[s_candPos]];
    setInfo(WifiState::Connecting, n.ssid, "");
    WiFi.disconnect(false, false);
    WiFi.begin(n.ssid, n.password[0] != '\0' ? n.password : nullptr);
    s_attemptMs = millis();
    WIFI_LOG("STA: connecting to \"%s\" (round %u, candidate %u/%u)\n", n.ssid,
             static_cast<unsigned>(s_rounds + 1), static_cast<unsigned>(s_candPos + 1),
             static_cast<unsigned>(s_candCount));
}

// Починає вибір мережі у фазі ph. forceSsid — спершу спробувати лише цю збережену мережу
// (примусове підключення); shownSsid — що показувати як «поточну» до першої спроби.
void startSelection(Phase ph, const char* forceSsid, const char* shownSsid) {
    s_phase = ph;
    s_rounds = 0;
    s_selectStartMs = millis();
    strlcpy(s_forceSsid, forceSsid != nullptr ? forceSsid : "", sizeof(s_forceSsid));
    s_rssi = 0;
    s_channel = 0;
    setInfo(WifiState::Connecting, shownSsid != nullptr ? shownSsid : "", "");
    startRound();
}

void startRound() {
    wipeSnapshot();
    const int8_t n = WifiNetworks::snapshot(s_snap, wifi_cfg::kMaxSavedNetworks);
    if (n < 0) {  // мʼютекс списку зайнятий: спробуємо за мить
        s_step = Step::Pause;
        s_stepMs = millis();
        return;
    }
    s_snapCount = static_cast<uint8_t>(n);
    if (s_snapCount == 0) {
        WIFI_LOG("STA: no saved networks, starting AP\n");
        enterAp();
        return;
    }

    // Примусове підключення: перший раунд складається лише з вибраної мережі.
    if (s_forceSsid[0] != '\0') {
        for (uint8_t i = 0; i < s_snapCount; ++i) {
            if (strcmp(s_snap[i].ssid, s_forceSsid) == 0) {
                s_cand[0] = i;
                s_candCount = 1;
                WIFI_LOG("STA: switching to \"%s\"\n", s_snap[i].ssid);
                break;
            }
        }
        s_forceSsid[0] = '\0';  // одноразово: далі звичайний вибір
    }

    if (s_candCount == 0 && s_snapCount > 1) {
        s_step = Step::Scan;  // кілька мереж: спершу дізнаємось, які з них видно
        s_scanStarted = false;
        return;
    }
    if (s_candCount == 0) {  // одна мережа: пряма спроба (як до P29)
        s_cand[0] = 0;
        s_candCount = 1;
    }
    s_candPos = 0;
    s_step = Step::Try;
    beginAttempt();
}

void connectedNow() {
    WIFI_LOG("STA: connected after %u failed round(s)\n", static_cast<unsigned>(s_rounds));
    onConnected();
    s_phase = Phase::Monitor;
    wipeSnapshot();
}

void roundFailed() {
    ++s_rounds;
    if (s_phase == Phase::FirstConnect) {
        const bool budgetSpent = elapsed(millis(), s_selectStartMs, wifi_cfg::kApBudgetMs);
        WIFI_LOG("STA: round %u/%u failed\n", static_cast<unsigned>(s_rounds),
                 static_cast<unsigned>(defaults::kWifiRetryBeforeAp));
        if (s_rounds >= defaults::kWifiRetryBeforeAp || budgetSpent) {
            WIFI_LOG("STA: giving up (%s), starting AP\n",
                     budgetSpent ? "time budget" : "retries");
            enterAp();
            return;
        }
        startRound();
        return;
    }
    // Reconnect: без AP, пауза й новий раунд.
    wipeSnapshot();
    s_step = Step::Pause;
    s_stepMs = millis();
}

void scanStep() {
    if (!s_scanStarted) {
        // Чужий скан (запит з API) ще триває: дочекаємось, потім стартуємо свій.
        if (scanningNow() && !s_scanOwnerEngine) {
            pollScan();
            return;
        }
        startScan(wifi_cfg::kScanChannelStaMs, true);
        s_scanStarted = true;
        return;
    }
    if (scanningNow()) {
        pollScan();
        return;
    }
    // Скан завершено (успішно чи ні): кандидати = видимі збережені за пріоритетом.
    s_candCount = 0;
    for (uint8_t i = 0; i < s_snapCount && s_candCount < wifi_cfg::kMaxCandidatesPerRound; ++i) {
        if (s_seen[i]) s_cand[s_candCount++] = i;
    }
    if (s_candCount == 0) {  // нічого не видно (приховані SSID або скан не вдався): напряму
        WIFI_LOG("STA: no saved network visible in scan, trying them directly\n");
        for (uint8_t i = 0; i < s_snapCount && s_candCount < wifi_cfg::kMaxCandidatesPerRound; ++i) {
            s_cand[s_candCount++] = i;
        }
    }
    s_candPos = 0;
    s_step = Step::Try;
    beginAttempt();
}

void tryStep() {
    if (WiFi.status() == WL_CONNECTED) {
        connectedNow();
        return;
    }
    const wl_status_t st = WiFi.status();
    const uint32_t timeoutMs = (s_phase == Phase::Reconnect) ? wifi_cfg::kReconnectAttemptTimeoutMs
                                                              : wifi_cfg::kConnectTimeoutMs;
    const bool timeout = elapsed(millis(), s_attemptMs, timeoutMs);
    if (st != WL_CONNECT_FAILED && !timeout) {
        return;
    }
    WIFI_LOG("STA: \"%s\" failed (%s)\n", s_snap[s_cand[s_candPos]].ssid, statusName(st));
    WiFi.disconnect(false, false);
    ++s_candPos;
    // Бюджет діє лише до першого підключення: нових кандидатів у цьому раунді не починаємо.
    const bool budgetSpent = (s_phase == Phase::FirstConnect) &&
                             elapsed(millis(), s_selectStartMs, wifi_cfg::kApBudgetMs);
    if (s_candPos < s_candCount && !budgetSpent) {
        beginAttempt();
        return;
    }
    roundFailed();
}

void pauseStep() {
    if (elapsed(millis(), s_stepMs, wifi_cfg::kReconnectPeriodMs)) {
        startRound();
    }
}

// Запит з API на примусове підключення (лише з задачі). Повертає true і SSID, якщо запит був.
bool takeConnectRequest(char* out, size_t cap) {
    Lock l;
    if (!l.ok() || !s_apiConnectPending) return false;
    strlcpy(out, s_apiConnectSsid, cap);
    s_apiConnectPending = false;
    return true;
}

// Виконується в Monitor і Reconnect. Відповідь клієнту вже відправлена обробником; пауза
// kConnectReplyFlushMs дає їй дійти до того, як з'єднання буде розірване.
bool handleConnectRequest() {
    char ssid[wifi_cfg::kSsidMax + 1];
    if (!takeConnectRequest(ssid, sizeof(ssid))) return false;
    WIFI_LOG("STA: switch requested to \"%s\"\n", ssid);
    abortScan();
    vTaskDelay(pdMS_TO_TICKS(wifi_cfg::kConnectReplyFlushMs));
    WiFi.disconnect(false, false);
    startSelection(Phase::Reconnect, ssid, ssid);
    return true;
}

void connectingTick() {
    if (s_phase == Phase::Reconnect && handleConnectRequest()) {
        return;
    }
    switch (s_step) {
        case Step::Scan:  scanStep();  break;
        case Step::Try:   tryStep();   break;
        case Step::Pause: pauseStep(); break;
    }
}

void staMonitorTick() {
    if (handleConnectRequest()) {
        return;
    }
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        WIFI_LOG("STA: connection lost\n");
        char saved[wifi_cfg::kSsidMax + 1];
        WifiManager::copyInfo(saved, sizeof(saved), nullptr, 0);  // SSID, що щойно працював
        abortScan();
        startSelection(Phase::Reconnect, "", saved);
        return;
    }
    if (elapsed(now, s_lastRssiMs, wifi_cfg::kRssiPeriodMs)) {
        s_lastRssiMs = now;
        s_rssi = WiFi.RSSI();
        s_channel = WiFi.channel();
    }

    // Скан за запитом API (POST /api/wifi/scan).
    bool scanReq = false;
    bool scanning = false;
    {
        Lock l;
        if (l.ok()) {
            scanReq = s_scanRequested;
            scanning = s_scanning;
        }
    }
    if (scanning) {
        pollScan();
    } else if (scanReq) {
        startScan(wifi_cfg::kScanChannelStaMs, false);
    }
}

void taskMain(void*) {
    WiFi.persistent(true);                 // «остання мережа» -> NVS (esp_wifi); список — wifi_networks
    WiFi.setHostname(defaults::kMdnsName);
    WiFi.setAutoReconnect(false);          // повтор робить ця задача, а не стек
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                  // енергозбереження Wi-Fi шкодить безперервному потоку

    if (s_forceReset) {
        WIFI_LOG("reset requested: erasing saved networks\n");
        resetCredentials();
        WifiNetworks::clear();  // [Prompt 29] усі збережені мережі (порожній список у NVS)
        enterAp();
    } else {
        if (!WifiNetworks::hasStoredList()) {
            migrateLegacyNetwork();  // [Prompt 29] стара одинична мережа -> запис №1
        }
        if (WifiNetworks::count() == 0) {
            WIFI_LOG("no saved network, starting AP\n");
            enterAp();
        } else {
            startSelection(Phase::FirstConnect, "", "");
        }
    }

    for (;;) {
        if (s_stopRequested && s_phase != Phase::Off) {  // [Prompt 28]
            shutdownRadio();
        }
        persistTick();  // [Prompt 29]
        switch (s_phase) {
            case Phase::FirstConnect:
            case Phase::Reconnect:    connectingTick(); break;
            case Phase::Monitor:      staMonitorTick(); break;
            case Phase::Ap:           apTick();         break;
            case Phase::Off:          break;  // [Prompt 28] нічого: Wi-Fi вимкнено
        }
        vTaskDelay(pdMS_TO_TICKS(s_phase == Phase::Ap ? wifi_cfg::kApPollMs
                                                      : wifi_cfg::kStaPollMs));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Публічний API
// ---------------------------------------------------------------------------
bool WifiManager::begin(bool forceReset) {
    if (s_task != nullptr) {
        return true;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        Serial.println("[WIFI] mutex create failed");
        return false;
    }
    if (!WifiNetworks::begin()) {  // [Prompt 29] читає список із NVS
        Serial.println("[WIFI] network list init failed");
        return false;
    }
    s_forceReset = forceReset;
    const BaseType_t ok = xTaskCreatePinnedToCore(taskMain, "wifi", wifi_cfg::kTaskStackBytes,
                                                  nullptr, wifi_cfg::kTaskPriority, &s_task,
                                                  wifi_cfg::kTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        Serial.println("[WIFI] task create failed");
        return false;
    }
    return true;
}

// [Prompt 28] ДОДАНО
void WifiManager::stop() {
    if (s_task == nullptr) {
        return;  // begin() не викликано: вимикати нічого
    }
    s_stopRequested = true;
}

WifiState WifiManager::state() { return s_state; }

bool WifiManager::isConnected() { return s_state == WifiState::Connected; }

bool WifiManager::isApMode() {
    const WifiState st = s_state;
    return st == WifiState::ApMode || st == WifiState::ApClientConnected;
}

const char* WifiManager::ssid() { return s_ssid; }

const char* WifiManager::ipAddress() { return s_ip; }

void WifiManager::copyInfo(char* ssidOut, size_t ssidCap, char* ipOut, size_t ipCap) {
    if (ssidOut != nullptr && ssidCap > 0) memset(ssidOut, 0, ssidCap);
    if (ipOut != nullptr && ipCap > 0) memset(ipOut, 0, ipCap);
    Lock l;
    if (!l.ok()) {
        return;  // порожні рядки: наступна синхронізація виправить
    }
    if (ssidOut != nullptr && ssidCap > 0) strlcpy(ssidOut, s_ssid, ssidCap);
    if (ipOut != nullptr && ipCap > 0) strlcpy(ipOut, s_ip, ipCap);
}

int32_t WifiManager::rssi() { return (s_state == WifiState::Connected) ? s_rssi : 0; }

// [Prompt 29] ДОДАНО
int32_t WifiManager::channel() { return (s_state == WifiState::Connected) ? s_channel : 0; }

WifiRequest WifiManager::requestScan() {
    if (s_task == nullptr || s_state != WifiState::Connected || s_phase != Phase::Monitor) {
        return WifiRequest::NotReady;
    }
    Lock l;
    if (!l.ok()) return WifiRequest::LockBusy;
    if (s_scanning || s_scanRequested) return WifiRequest::ScanBusy;
    s_scanRequested = true;
    return WifiRequest::Accepted;
}

WifiScanState WifiManager::scanState(uint32_t* ageMsOut) {
    if (ageMsOut != nullptr) *ageMsOut = 0;
    Lock l;
    if (!l.ok()) return WifiScanState::Running;  // клієнт опитає ще раз
    if (s_scanning || s_scanRequested) return WifiScanState::Running;
    switch (s_scanResult) {
        case ScanResult::Done:
            if (ageMsOut != nullptr) *ageMsOut = static_cast<uint32_t>(millis() - s_scanDoneMs);
            return WifiScanState::Done;
        case ScanResult::Failed: return WifiScanState::Failed;
        case ScanResult::None:   break;
    }
    return WifiScanState::Idle;
}

uint8_t WifiManager::copyScan(WifiScanEntry* out, uint8_t cap) {
    Lock l;
    if (!l.ok() || s_scanResult != ScanResult::Done) return 0;
    const uint8_t n = (s_netCount < cap) ? s_netCount : cap;
    for (uint8_t i = 0; i < n; ++i) {
        strlcpy(out[i].ssid, s_nets[i].ssid, sizeof(out[i].ssid));
        out[i].rssi = s_nets[i].rssi;
        out[i].channel = s_nets[i].channel;
        out[i].secure = s_nets[i].secure;
    }
    return n;
}

WifiRequest WifiManager::requestConnect(uint8_t index) {
    const Phase ph = s_phase;
    if (s_task == nullptr || (ph != Phase::Monitor && ph != Phase::Reconnect)) {
        return WifiRequest::NotReady;
    }
    WifiNetInfo nets[wifi_cfg::kMaxSavedNetworks];
    const int8_t n = WifiNetworks::list(nets, wifi_cfg::kMaxSavedNetworks);
    if (n < 0) return WifiRequest::LockBusy;
    if (index >= static_cast<uint8_t>(n)) return WifiRequest::NoSuchNetwork;

    Lock l;
    if (!l.ok()) return WifiRequest::LockBusy;
    if (s_apiConnectPending) return WifiRequest::Pending;
    if (ph == Phase::Monitor && s_state == WifiState::Connected &&
        strcmp(s_ssid, nets[index].ssid) == 0) {
        return WifiRequest::AlreadyCurrent;
    }
    strlcpy(s_apiConnectSsid, nets[index].ssid, sizeof(s_apiConnectSsid));
    s_apiConnectPending = true;
    return WifiRequest::Accepted;
}
