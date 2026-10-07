// WifiManager (Prompt 12): STA -> (3 невдачі | немає мережі | скидання) -> AP + captive portal.
//
// Усі виклики WiFi.* робить ОДНА внутрішня задача. HTTP-обробники порталу (задача
// AsyncTCP) лише читають кеш скану й залишають запити через прапорці під мʼютексом.
//
// Облікові дані: WiFi.begin(ssid, pass) із persistent=true зберігає їх у NVS (esp_wifi),
// WiFi.begin() без аргументів підключається до останньої збереженої мережі.
//
// [Prompt 28] stop(): запит на повне вимкнення Wi-Fi (офлайн-режим). Виконує задача:
// DNS стоп, HTTP-портал end(), softAP/STA вниз, WiFi.mode(WIFI_OFF), Phase::Off.

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

#if WIFI_MANAGER_DEBUG
#define WIFI_LOG(...) Serial.printf("[WIFI] " __VA_ARGS__)
#else
#define WIFI_LOG(...) \
    do {              \
    } while (0)
#endif

namespace {

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
    Reconnect,     // звʼязок втрачено під час роботи: простий повтор без переходу в AP
    Ap,            // AP + captive portal
    Off,           // [Prompt 28] Wi-Fi вимкнено (офлайн): задача лише спить
};

enum class PortalConn : uint8_t { Idle, Trying, Failed };

struct Net {
    char ssid[wifi_cfg::kSsidMax + 1];
    int8_t rssi;
    bool secure;
};

SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
bool s_forceReset = false;
volatile bool s_stopRequested = false;  // [Prompt 28] ставить stop(), знімає задача

// --- Спільне (захищене s_lock; s_state/s_rssi читаються без нього як атомарні) ---
volatile WifiState s_state = WifiState::Connecting;
volatile int32_t s_rssi = 0;
char s_ssid[wifi_cfg::kSsidMax + 1] = "";
char s_ip[16] = "";

Net s_nets[wifi_cfg::kMaxNetworks];
uint8_t s_netCount = 0;
bool s_scanning = false;
bool s_scanRequested = false;

bool s_connectRequested = false;
char s_reqSsid[wifi_cfg::kSsidMax + 1] = "";
char s_reqPass[wifi_cfg::kPassMax + 1] = "";
PortalConn s_portalConn = PortalConn::Idle;

// --- Лише задача WifiManager ---
Phase s_phase = Phase::FirstConnect;
uint8_t s_attempts = 0;
uint32_t s_attemptStartMs = 0;
uint32_t s_lastReconnectMs = 0;
uint32_t s_lastRssiMs = 0;
uint32_t s_lastApCheckMs = 0;
uint32_t s_scanStartMs = 0;
uint32_t s_reconnectCount = 0;
char s_apIpStr[16] = "";
char s_portalUrl[32] = "";

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

// Збережена в NVS мережа STA. НЕ WiFi.SSID(): у Arduino-ESP32 3.x воно повертає
// SSID поточного підключення (порожній рядок, поки зʼєднання немає).
// Викликати після WiFi.mode(WIFI_STA) (esp_wifi вже ініціалізовано).
bool savedSsid(char* out, size_t cap) {
    out[0] = '\0';
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || conf.sta.ssid[0] == '\0') {
        return false;
    }
    const size_t n = strnlen(reinterpret_cast<const char*>(conf.sta.ssid), sizeof(conf.sta.ssid));
    const size_t m = (n < cap - 1) ? n : cap - 1;
    memcpy(out, conf.sta.ssid, m);
    out[m] = '\0';
    return true;
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

    if (ssid.length() == 0 || ssid.length() > wifi_cfg::kSsidMax ||
        pass.length() > wifi_cfg::kPassMax || (pass.length() > 0 && pass.length() < 8)) {
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
void startScan() {
    {
        Lock l;
        s_scanRequested = false;
        s_scanning = true;
    }
    WiFi.scanDelete();
    const int r = WiFi.scanNetworks(true);
    s_scanStartMs = millis();
    if (r == WIFI_SCAN_FAILED) {
        Lock l;
        s_scanning = false;
        WIFI_LOG("scan start FAILED\n");
        return;
    }
    WIFI_LOG("scan started\n");
}

void pollScan() {
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        if (elapsed(millis(), s_scanStartMs, wifi_cfg::kScanTimeoutMs)) {
            WiFi.scanDelete();
            Lock l;
            s_scanning = false;
            WIFI_LOG("scan timeout\n");
        }
        return;
    }
    if (n < 0) {
        Lock l;
        s_scanning = false;
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
            }
            continue;
        }
        if (cnt < wifi_cfg::kMaxNetworks) {
            strlcpy(tmp[cnt].ssid, name.c_str(), sizeof(tmp[cnt].ssid));
            tmp[cnt].rssi = rssi;
            tmp[cnt].secure = secure;
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
    WIFI_LOG("scan done: %u networks\n", static_cast<unsigned>(cnt));
}

// ---------------------------------------------------------------------------
// STA
// ---------------------------------------------------------------------------
void onConnected() {
    const String ssid = WiFi.SSID();
    const String ip = WiFi.localIP().toString();
    s_rssi = WiFi.RSSI();
    s_lastRssiMs = millis();
    setInfo(WifiState::Connected, ssid.c_str(), ip.c_str());
    WIFI_LOG("STA connected: \"%s\" ip=%s rssi=%d\n", ssid.c_str(), ip.c_str(),
             static_cast<int>(s_rssi));
    WIFI_LOG("task stack free (min): %u bytes\n",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    MdnsManager::begin();  // ідемпотентно: реально стартує лише один раз
}

void startFirstConnect(const char* saved) {
    s_attempts = 0;
    s_phase = Phase::FirstConnect;
    setInfo(WifiState::Connecting, saved, "");
    WiFi.begin();  // остання збережена мережа
    s_attemptStartMs = millis();
    WIFI_LOG("STA: connecting to \"%s\" (attempt 1/%u)\n", saved,
             static_cast<unsigned>(defaults::kWifiRetryBeforeAp));
}

void resetCredentials() {
    WiFi.disconnect(false, true);  // STA вже запущено; true = стерти збережені дані
    char left[wifi_cfg::kSsidMax + 1];
    if (!savedSsid(left, sizeof(left))) {
        WIFI_LOG("saved network erased\n");
    } else {
        WIFI_LOG("WARNING: saved network still present after erase: \"%s\"\n", left);
    }
}

// ---------------------------------------------------------------------------
// AP + captive portal
// ---------------------------------------------------------------------------
void enterAp() {
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
            s_portalConn = PortalConn::Idle;
            memset(s_reqPass, 0, sizeof(s_reqPass));
        }
    }
    s_dns.stop();
    if (s_server != nullptr) {
        s_server->end();  // об'єкт лишаємо (його могли б ще використовувати обробники AsyncTCP)
    }
    WiFi.scanDelete();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    s_rssi = 0;
    setInfo(WifiState::Off, "", "");
    s_phase = Phase::Off;
    s_stopRequested = false;
    WIFI_LOG("stop: Wi-Fi is off\n");
}

// Підключення з порталу: WiFi.begin(ssid, pass) (бібліотека сама збереже дані) ->
// очікування -> перезапуск. Чистий перехід у STA на наступному завантаженні
// простіший за перемикання режиму на льоту (AP, DNS і HTTP-сервер лишились би живі).
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
    memset(pass, 0, sizeof(pass));

    const uint32_t start = millis();
    wl_status_t st;
    do {
        vTaskDelay(pdMS_TO_TICKS(100));
        st = WiFi.status();
    } while (st != WL_CONNECTED && st != WL_CONNECT_FAILED &&
             !elapsed(millis(), start, wifi_cfg::kPortalConnectTimeoutMs));

    if (st == WL_CONNECTED) {
        char chk[wifi_cfg::kSsidMax + 1];
        WIFI_LOG("portal: connected, saved=%s; restarting\n", savedSsid(chk, sizeof(chk)) ? chk : "NO");
        vTaskDelay(pdMS_TO_TICKS(wifi_cfg::kRestartDelayMs));
        ESP.restart();
        return;
    }

    WIFI_LOG("portal: failed (%s), staying in AP\n", statusName(st));
    WiFi.disconnect(false, true);  // не лишати в NVS мережу, яка не підключилась
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
        startScan();
    }
}

// ---------------------------------------------------------------------------
// Основний цикл
// ---------------------------------------------------------------------------
void staFirstConnectTick() {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        onConnected();
        s_phase = Phase::Monitor;
        return;
    }
    const bool timeout = elapsed(millis(), s_attemptStartMs, wifi_cfg::kConnectTimeoutMs);
    if (st != WL_CONNECT_FAILED && !timeout) {
        return;
    }

    ++s_attempts;
    WIFI_LOG("STA: attempt %u/%u failed (%s)\n", static_cast<unsigned>(s_attempts),
             static_cast<unsigned>(defaults::kWifiRetryBeforeAp), statusName(st));
    if (s_attempts >= defaults::kWifiRetryBeforeAp) {
        WIFI_LOG("STA: giving up, starting AP\n");
        enterAp();
        return;
    }
    WiFi.disconnect(false, false);
    WiFi.begin();
    s_attemptStartMs = millis();
    WIFI_LOG("STA: connecting (attempt %u/%u)\n", static_cast<unsigned>(s_attempts + 1),
             static_cast<unsigned>(defaults::kWifiRetryBeforeAp));
}

void staMonitorTick() {
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        WIFI_LOG("STA: connection lost\n");
        char saved[wifi_cfg::kSsidMax + 1];
        WifiManager::copyInfo(saved, sizeof(saved), nullptr, 0);  // SSID, що щойно працював
        setInfo(WifiState::Connecting, saved, "");
        s_rssi = 0;
        s_phase = Phase::Reconnect;
        s_lastReconnectMs = now - wifi_cfg::kReconnectPeriodMs;  // перша спроба одразу
        s_reconnectCount = 0;
        return;
    }
    if (elapsed(now, s_lastRssiMs, wifi_cfg::kRssiPeriodMs)) {
        s_lastRssiMs = now;
        s_rssi = WiFi.RSSI();
    }
}

void staReconnectTick() {
    if (WiFi.status() == WL_CONNECTED) {
        WIFI_LOG("STA: reconnected after %u attempts\n", static_cast<unsigned>(s_reconnectCount));
        onConnected();
        s_phase = Phase::Monitor;
        return;
    }
    const uint32_t now = millis();
    if (elapsed(now, s_lastReconnectMs, wifi_cfg::kReconnectPeriodMs)) {
        s_lastReconnectMs = now;
        ++s_reconnectCount;
        WIFI_LOG("STA: reconnect attempt %u\n", static_cast<unsigned>(s_reconnectCount));
        WiFi.reconnect();
    }
}

void taskMain(void*) {
    WiFi.persistent(true);                 // облікові дані -> NVS (esp_wifi)
    WiFi.setHostname(defaults::kMdnsName);
    WiFi.setAutoReconnect(false);          // повтор робить ця задача, а не стек
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                  // енергозбереження Wi-Fi шкодить безперервному потоку

    if (s_forceReset) {
        WIFI_LOG("reset requested: erasing saved network\n");
        resetCredentials();
        enterAp();
    } else {
        char saved[wifi_cfg::kSsidMax + 1];
        if (!savedSsid(saved, sizeof(saved))) {
            WIFI_LOG("no saved network, starting AP\n");
            enterAp();
        } else {
            startFirstConnect(saved);
        }
    }

    for (;;) {
        if (s_stopRequested && s_phase != Phase::Off) {  // [Prompt 28]
            shutdownRadio();
        }
        switch (s_phase) {
            case Phase::FirstConnect: staFirstConnectTick(); break;
            case Phase::Monitor:      staMonitorTick();      break;
            case Phase::Reconnect:    staReconnectTick();    break;
            case Phase::Ap:           apTick();              break;
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
