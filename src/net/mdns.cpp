#include "net/mdns.h"

#include <Arduino.h>
#include <ESPmDNS.h>

#include "config/defaults.h"
#include "config/wifi_config.h"

namespace {
bool s_started = false;
}  // namespace

bool MdnsManager::begin() {
    if (s_started) {
        return true;
    }
    if (!MDNS.begin(defaults::kMdnsName)) {
        Serial.println("[MDNS] begin failed");
        return false;
    }
    MDNS.addService("http", "tcp", wifi_cfg::kMdnsHttpPort);
    s_started = true;
    Serial.printf("[MDNS] %s.local, _http._tcp port %u\n", defaults::kMdnsName,
                  static_cast<unsigned>(wifi_cfg::kMdnsHttpPort));
    return true;
}
