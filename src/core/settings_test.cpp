// Serial-тест Settings. Протокол — у settings_test.h.

#include "core/settings_test.h"

#include "config/settings_config.h"

#if SETTINGS_TEST

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "config/defaults.h"
#include "core/settings.h"

namespace {

constexpr const char* kPrefix = "set.";

enum class Field : uint8_t {
    Proc, Bright, Flip, Bass, Treble, Balance, Loud, Input, Station, Vol, Mute, Name, RampVol,
};

struct Edit {
    Field field;
    int32_t value;
    uint8_t index;
    char text[32];
};

// Виконується під мʼютексом SettingsStore::modify().
void applyEdit(Settings& s, void* ctx) {
    const Edit& e = *static_cast<const Edit*>(ctx);
    switch (e.field) {
        case Field::Proc:     s.processorType = static_cast<uint8_t>(e.value); break;
        case Field::Bright:   s.brightness = static_cast<uint8_t>(e.value); break;
        case Field::Flip:     s.displayFlipped = (e.value != 0); break;
        case Field::Bass:     s.bass = static_cast<int8_t>(e.value); break;
        case Field::Treble:   s.treble = static_cast<int8_t>(e.value); break;
        case Field::Balance:  s.balance = static_cast<int8_t>(e.value); break;
        case Field::Loud:     s.loudness = (e.value != 0); break;
        case Field::Input:    s.lastInput = static_cast<uint8_t>(e.value); break;
        case Field::Station:  s.lastStation = static_cast<uint16_t>(e.value); break;
        case Field::Vol:      s.lastVolume = static_cast<int8_t>(e.value); break;
        case Field::RampVol:  s.lastVolume = static_cast<int8_t>(e.value); break;
        case Field::Mute:     s.lastMute = (e.value != 0); break;
        case Field::Name:
            snprintf(s.inputNames[e.index], sizeof(s.inputNames[e.index]), "%s", e.text);
            break;
    }
}

struct NumericCmd {
    const char* name;
    Field field;
    long minV;
    long maxV;
};

const NumericCmd kNumeric[] = {
    {"proc",    Field::Proc,    0, 1},
    {"bright",  Field::Bright,  0, 255},
    {"flip",    Field::Flip,    0, 1},
    {"bass",    Field::Bass,    -128, 127},
    {"treble",  Field::Treble,  -128, 127},
    {"balance", Field::Balance, -128, 127},
    {"loud",    Field::Loud,    0, 1},
    {"input",   Field::Input,   0, defaults::kInputCount - 1},
    {"station", Field::Station, 0, 65535},
    {"vol",     Field::Vol,     -128, 127},
    {"mute",    Field::Mute,    0, 1},
};

void printHelp() {
    Serial.println("[SET] commands (all start with 'set.'):");
    Serial.println("[SET]   set.show | set.save | set.ramp | set.reset | set.erase | set.help");
    Serial.println("[SET]   set.proc 0|1   set.bright 0..255   set.flip 0|1   set.loud 0|1");
    Serial.println("[SET]   set.bass|treble|balance|vol -128..127   set.mute 0|1");
    Serial.printf("[SET]   set.input 0..%u   set.station 0..65535   set.name <idx> <text>\n",
                  static_cast<unsigned>(defaults::kInputCount - 1));
}

void printShow() {
    const Settings s = SettingsStore::snapshot();
    Serial.println("[SET] ---- settings ----");
    Serial.printf("[SET] processorType=%u (%s)\n", static_cast<unsigned>(s.processorType),
                  s.processorType == 0 ? "TDA7318" : "PT2313L");
    for (uint8_t i = 0; i < defaults::kInputCount; ++i) {
        Serial.printf("[SET] inputNames[%u]=\"%s\"\n", static_cast<unsigned>(i), s.inputNames[i]);
    }
    Serial.printf("[SET] brightness=%u displayFlipped=%d\n", static_cast<unsigned>(s.brightness),
                  s.displayFlipped ? 1 : 0);
    Serial.printf("[SET] bass=%d treble=%d balance=%d loudness=%d\n", static_cast<int>(s.bass),
                  static_cast<int>(s.treble), static_cast<int>(s.balance), s.loudness ? 1 : 0);
    Serial.printf("[SET] lastInput=%u lastStation=%u lastVolume=%d lastMute=%d\n",
                  static_cast<unsigned>(s.lastInput), static_cast<unsigned>(s.lastStation),
                  static_cast<int>(s.lastVolume), s.lastMute ? 1 : 0);
    Serial.printf("[SET] dirty=%d nvsWrites=%u\n", SettingsStore::isDirty() ? 1 : 0,
                  static_cast<unsigned>(SettingsStore::writeCount()));
}

// Розбір цілого: усе слово має бути числом.
bool parseLong(const char* str, long& out) {
    if (str == nullptr || *str == '\0') {
        return false;
    }
    char* end = nullptr;
    out = strtol(str, &end, 10);
    return end != str && *end == '\0';
}

void reportScheduled(const char* what, long value) {
    Serial.printf("[SET] %s = %ld (pending, NVS write in ~%u ms; 'set.save' to force)\n", what,
                  value, static_cast<unsigned>(settings_cfg::kSaveDebounceMs));
}

void runRamp() {
    const uint32_t writesBefore = SettingsStore::writeCount();
    Serial.printf("[SET] ramp: %u requestSave() calls, %u ms apart\n",
                  static_cast<unsigned>(settings_cfg::kTestRampSteps),
                  static_cast<unsigned>(settings_cfg::kTestRampStepMs));
    for (uint16_t i = 0; i < settings_cfg::kTestRampSteps; ++i) {
        Edit e{};
        e.field = Field::RampVol;
        e.value = i % 100;
        SettingsStore::modify(applyEdit, &e);
        vTaskDelay(pdMS_TO_TICKS(settings_cfg::kTestRampStepMs));
    }
    Serial.printf("[SET] ramp done: NVS writes during ramp = %u (expected 0), dirty=%d\n",
                  static_cast<unsigned>(SettingsStore::writeCount() - writesBefore),
                  SettingsStore::isDirty() ? 1 : 0);
    Serial.println("[SET] one write should follow after the debounce; check with 'set.show'");
}

void testTask(void*) {
    char buf[settings_cfg::kTestLineMax];
    size_t len = 0;
    bool overflow = false;

    for (;;) {
        while (Serial.available() > 0) {
            const int c = Serial.read();
            if (c == '\r' || c == '\n') {
                if (len > 0 && !overflow) {
                    buf[len] = '\0';
                    SettingsTest::handleLine(buf);
                }
                len = 0;
                overflow = false;
            } else if (len < sizeof(buf) - 1) {
                buf[len++] = static_cast<char>(c);
            } else {
                overflow = true;  // надто довгий рядок — відкидаємо цілком
            }
        }
        vTaskDelay(pdMS_TO_TICKS(settings_cfg::kTestPollMs));
    }
}

}  // namespace

bool SettingsTest::begin() {
    const BaseType_t ok = xTaskCreatePinnedToCore(
        testTask, "settings_test", settings_cfg::kTestTaskStackBytes, nullptr,
        settings_cfg::kTestTaskPriority, nullptr, settings_cfg::kTestTaskCore);
    return ok == pdPASS;
}

void SettingsTest::handleLine(const char* line) {
    if (line == nullptr || strncmp(line, kPrefix, strlen(kPrefix)) != 0) {
        return;  // чужі команди (інші тестові режими) ігноруємо мовчки
    }

    // Локальна копія: розбираємо на команду й залишок.
    char buf[settings_cfg::kTestLineMax];
    snprintf(buf, sizeof(buf), "%s", line + strlen(kPrefix));

    char* cmd = buf;
    char* args = strchr(buf, ' ');
    if (args != nullptr) {
        *args++ = '\0';
        while (*args == ' ') {
            ++args;
        }
    }
    const char* argStr = (args != nullptr) ? args : "";

    if (strcmp(cmd, "show") == 0) {
        printShow();
    } else if (strcmp(cmd, "help") == 0) {
        printHelp();
    } else if (strcmp(cmd, "save") == 0) {
        Serial.printf("[SET] save: %s\n", SettingsStore::save() ? "ok" : "FAILED");
    } else if (strcmp(cmd, "ramp") == 0) {
        runRamp();
    } else if (strcmp(cmd, "reset") == 0) {
        Serial.printf("[SET] reset to defaults: %s (processorType applies after reboot)\n",
                      SettingsStore::resetToDefaults() ? "ok" : "FAILED");
    } else if (strcmp(cmd, "erase") == 0) {
        Serial.printf("[SET] erase stored key: %s (reboot to test first-run path)\n",
                      SettingsStore::eraseStored() ? "ok" : "FAILED");
    } else if (strcmp(cmd, "name") == 0) {
        // set.name <idx> <текст>
        char* text = strchr(const_cast<char*>(argStr), ' ');
        long idx = 0;
        if (text != nullptr) {
            *text++ = '\0';
        }
        if (text == nullptr || *text == '\0' || !parseLong(argStr, idx) || idx < 0 ||
            idx >= defaults::kInputCount) {
            Serial.printf("[SET] usage: set.name <0..%u> <text>\n",
                          static_cast<unsigned>(defaults::kInputCount - 1));
            return;
        }
        Edit e{};
        e.field = Field::Name;
        e.index = static_cast<uint8_t>(idx);
        snprintf(e.text, sizeof(e.text), "%s", text);
        SettingsStore::modify(applyEdit, &e);
        Serial.printf("[SET] inputNames[%ld] = \"%s\" (pending)\n", idx, e.text);
    } else {
        for (const NumericCmd& n : kNumeric) {
            if (strcmp(cmd, n.name) != 0) {
                continue;
            }
            long v = 0;
            if (!parseLong(argStr, v) || v < n.minV || v > n.maxV) {
                Serial.printf("[SET] usage: set.%s <%ld..%ld>\n", n.name, n.minV, n.maxV);
                return;
            }
            Edit e{};
            e.field = n.field;
            e.value = static_cast<int32_t>(v);
            SettingsStore::modify(applyEdit, &e);
            reportScheduled(n.name, v);
            return;
        }
        Serial.printf("[SET] unknown command 'set.%s' (try set.help)\n", cmd);
    }
}

#else  // !SETTINGS_TEST

bool SettingsTest::begin() {
    return false;
}

void SettingsTest::handleLine(const char*) {}

#endif  // SETTINGS_TEST
