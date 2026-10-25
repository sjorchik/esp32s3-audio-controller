#include "stations/station_store_test.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

#include "config/station_store_config.h"
#include "stations/station_store.h"

#if STATION_STORE_TEST

namespace cfg = station_store_cfg;

namespace {

bool s_started = false;
char s_line[cfg::kTestLineMax];  // BSS: не навантажує стек задачі
size_t s_len = 0;
bool s_overflow = false;

void printHelp() {
    Serial.println("[ST-TEST] commands:");
    Serial.println("  st.list");
    Serial.println("  st.export");
    Serial.println("  st.reset            (removes /stations.json; reseed after reboot)");
    Serial.println("  st.import <json>    e.g. st.import [{\"name\":\"A\",\"url\":\"http://x.y/z\"}]");
}

void cmdList() {
    const size_t n = StationStore::count();
    Serial.printf("[ST-TEST] %u stations\n", static_cast<unsigned>(n));
    Station st;
    for (size_t i = 0; i < n; ++i) {
        if (StationStore::get(i, st)) {
            Serial.printf("  %u: %s | %s\n", static_cast<unsigned>(i), st.name, st.url);
        }
    }
}

void cmdExport() {
    if (!StationStore::exportJson(cfg::kTestExportPath)) {
        Serial.println("[ST-TEST] export failed");
        return;
    }
    File f = LittleFS.open(cfg::kTestExportPath, "r");
    if (!f) {
        Serial.println("[ST-TEST] cannot reopen export file");
        return;
    }
    uint8_t chunk[128];
    while (f.available()) {
        const size_t r = f.read(chunk, sizeof(chunk));
        if (r == 0) break;
        Serial.write(chunk, r);
    }
    f.close();
    LittleFS.remove(cfg::kTestExportPath);
}

void cmdReset() {
    if (LittleFS.exists(cfg::kStoragePath)) {
        LittleFS.remove(cfg::kStoragePath);
        Serial.println("[ST-TEST] storage file removed; reboot to reseed "
                       "(list in RAM is unchanged until then)");
    } else {
        Serial.println("[ST-TEST] storage file does not exist");
    }
}

void cmdImport(const char* json) {
    while (*json == ' ') ++json;
    if (*json == '\0') {
        Serial.println("[ST-TEST] usage: st.import <json on one line>");
        return;
    }
    File f = LittleFS.open(cfg::kTestImportPath, "w");
    if (!f) {
        Serial.println("[ST-TEST] cannot create temp file");
        return;
    }
    const size_t len = strlen(json);
    const size_t w = f.write(reinterpret_cast<const uint8_t*>(json), len);
    f.close();
    if (w != len) {
        Serial.println("[ST-TEST] temp file write failed");
        LittleFS.remove(cfg::kTestImportPath);
        return;
    }
    const bool ok = StationStore::importJson(cfg::kTestImportPath);
    LittleFS.remove(cfg::kTestImportPath);
    Serial.printf("[ST-TEST] import %s, now %u stations\n", ok ? "OK" : "FAILED",
                  static_cast<unsigned>(StationStore::count()));
}

void handleLine(char* line) {
    if (strncmp(line, "st.", 3) != 0) return;  // чужі рядки мовчки ігноруємо
    if (strcmp(line, "st.help") == 0) {
        printHelp();
    } else if (strcmp(line, "st.list") == 0) {
        cmdList();
    } else if (strcmp(line, "st.export") == 0) {
        cmdExport();
    } else if (strcmp(line, "st.reset") == 0) {
        cmdReset();
    } else if (strncmp(line, "st.import", 9) == 0 && (line[9] == ' ' || line[9] == '\0')) {
        cmdImport(line + 9);
    } else {
        Serial.println("[ST-TEST] unknown command (st.help)");
    }
}

void taskMain(void*) {
    for (;;) {
        while (Serial.available() > 0) {
            const int c = Serial.read();
            if (c < 0) break;
            if (c == '\n' || c == '\r') {
                if (s_overflow) {
                    Serial.println("[ST-TEST] line too long, ignored");
                } else if (s_len > 0) {
                    s_line[s_len] = '\0';
                    handleLine(s_line);
                }
                s_len = 0;
                s_overflow = false;
            } else if (s_len + 1 < sizeof(s_line)) {
                s_line[s_len++] = static_cast<char>(c);
            } else {
                s_overflow = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(cfg::kTestPollMs));
    }
}

}  // namespace

bool StationStoreTest::begin() {
    if (s_started) return true;
    const BaseType_t ok = xTaskCreatePinnedToCore(taskMain, "st_test", cfg::kTestTaskStackBytes,
                                                  nullptr, cfg::kTestTaskPriority, nullptr,
                                                  cfg::kTestTaskCore);
    if (ok != pdPASS) return false;
    s_started = true;
    return true;
}

#else

bool StationStoreTest::begin() { return false; }

#endif  // STATION_STORE_TEST
