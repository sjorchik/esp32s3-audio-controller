// Тестовий режим аудіоплеєра (Serial-протокол).
//
// Команда + Enter, регістр команди байдужий (аргументи — як є). Відповіді
// мають формат "[PTEST] ...".
//
//   help                    — ця довідка
//   play <N>                — відтворити тестову станцію N (1..kTestStationCount)
//   play <http(s)://...>    — відтворити довільний URL (напр. некоректний — для
//                             перевірки перепідключення)
//   stop                    — зупинити (скасовує перепідключення)
//   info                    — стан, метадані, лічильники, XSMT, Wi-Fi, памʼять
//
// [Prompt 12] Тимчасові команди `wifi`, `wifioff`, `wifion` та константи
// player_cfg::kTestWifiSsid/Pass прибрано: Wi-Fi тепер повністю належить
// net/wifi_manager (інакше обидва модулі смикали б WiFi.begin()/режим).
// Для відтворення потрібна мережа, налаштована через портал AudioCtrl-Setup.
//   norm <hex>              — прогнати байти ICY-рядка (hex, напр. CEEAE5E0ED)
//                             через визначення кодування й перекодування
//   vol <0..100>            — гучність AudioProcessor (якщо він переданий)
//   pmute <0|1>             — мʼют атенюаторів AudioProcessor (незалежний від XSMT)

#include "audio/audio_player_test.h"

#if AUDIO_PLAYER_TEST

#include <Arduino.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_player.h"
#include "audio/audio_processor.h"
#include "config/audio_player_config.h"
#include "config/pins.h"
#include "net/wifi_manager.h"  // [Prompt 12] ДОДАНО: лише читання стану Wi-Fi

namespace {

AudioProcessor* s_proc = nullptr;
TaskHandle_t s_task = nullptr;

void printHelp() {
    Serial.println("[PTEST] commands:");
    Serial.println("  play <N|url>   play test station N (1..) or an http(s) URL");
    Serial.println("  stop           stop playback");
    Serial.println("  info           state, metadata, counters, XSMT, Wi-Fi");
    Serial.println("  norm <hex>     test ICY encoding detection, e.g. norm CEEAE5E0ED");
    Serial.println("  vol <0..100>   AudioProcessor volume");
    Serial.println("  pmute <0|1>    AudioProcessor speaker mute (independent of XSMT)");
    Serial.println("  help           this text");
    Serial.println("[PTEST] test stations:");
    for (size_t i = 0; i < player_cfg::kTestStationCount; ++i) {
        Serial.printf("  %u: %s  %s\n", static_cast<unsigned>(i + 1),
                      player_cfg::kTestStationNames[i], player_cfg::kTestStationUrls[i]);
    }
}

void printInfo() {
    char station[player_cfg::kStationMax];
    char title[player_cfg::kTitleMax];
    const bool hasMeta = AudioPlayer::currentMetadata(station, sizeof(station), title,
                                                      sizeof(title));
    Serial.printf("[PTEST] state=%s playing=%d\n", AudioPlayer::stateName(AudioPlayer::state()),
                  AudioPlayer::isPlaying() ? 1 : 0);
    Serial.printf("[PTEST] meta=%s station=\"%s\" title=\"%s\"\n", hasMeta ? "yes" : "none",
                  station, title);
    Serial.printf("[PTEST] reconnects=%lu underruns=%lu http=%d\n",
                  static_cast<unsigned long>(AudioPlayer::reconnectCount()),
                  static_cast<unsigned long>(AudioPlayer::bufferUnderrunCount()),
                  AudioPlayer::lastHttpCode());
    Serial.printf("[PTEST] XSMT pin level=%d (0 = muted)\n", digitalRead(pins::kXsmt));
    char wifiSsid[33];
    char wifiIp[16];
    WifiManager::copyInfo(wifiSsid, sizeof(wifiSsid), wifiIp, sizeof(wifiIp));
    Serial.printf("[PTEST] wifi=%s heap=%u psram=%u\n",
                  WifiManager::isConnected() ? wifiIp : "down",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getFreePsram()));
}

void cmdPlay(const char* arg) {
    if (arg[0] == '\0') {
        Serial.println("[PTEST] usage: play <N|url>");
        return;
    }
    if (strncmp(arg, "http://", 7) == 0 || strncmp(arg, "https://", 8) == 0) {
        Serial.printf("[PTEST] play url -> %s\n", AudioPlayer::playUrl(arg) ? "queued" : "REJECTED");
        return;
    }
    char* end = nullptr;
    const long n = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || n < 1 ||
        n > static_cast<long>(player_cfg::kTestStationCount)) {
        Serial.printf("[PTEST] bad station (1..%u) or url\n",
                      static_cast<unsigned>(player_cfg::kTestStationCount));
        return;
    }
    const size_t idx = static_cast<size_t>(n - 1);
    Serial.printf("[PTEST] play %ld: %s -> %s\n", n, player_cfg::kTestStationNames[idx],
                  AudioPlayer::playUrl(player_cfg::kTestStationUrls[idx]) ? "queued" : "REJECTED");
}

void cmdNorm(const char* arg) {
    uint8_t bytes[96];
    size_t n = 0;
    int hi = -1;
    for (const char* p = arg; *p != '\0'; ++p) {
        if (*p == ' ') continue;
        if (!isxdigit(static_cast<unsigned char>(*p))) {
            Serial.println("[PTEST] norm: hex digits only");
            return;
        }
        const int v = (*p <= '9') ? (*p - '0') : ((*p | 0x20) - 'a' + 10);
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= sizeof(bytes) - 1) {
                Serial.println("[PTEST] norm: too long");
                return;
            }
            bytes[n++] = static_cast<uint8_t>((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0 || n == 0) {
        Serial.println("[PTEST] norm: need an even number of hex digits");
        return;
    }
    bytes[n] = 0;
    char out[player_cfg::kTitleMax];
    const char* enc = AudioPlayer::normalizeIcy(reinterpret_cast<const char*>(bytes), out,
                                                sizeof(out));
    Serial.printf("[PTEST] norm: %u bytes, encoding=%s, utf8=\"%s\"\n",
                  static_cast<unsigned>(n), enc, out);
}

void cmdVol(const char* arg) {
    if (s_proc == nullptr) {
        Serial.println("[PTEST] no audio processor");
        return;
    }
    const long v = strtol(arg, nullptr, 10);
    Serial.printf("[PTEST] vol %ld -> %s\n", v,
                  s_proc->setVolume(static_cast<int8_t>(v)) ? "OK" : "FAIL");
}

void cmdPmute(const char* arg) {
    if (s_proc == nullptr) {
        Serial.println("[PTEST] no audio processor");
        return;
    }
    const bool m = (arg[0] == '1');
    Serial.printf("[PTEST] pmute %d -> %s\n", m ? 1 : 0, s_proc->setMute(m) ? "OK" : "FAIL");
}

void execute(char* line) {
    // Розділяємо на команду та аргумент (аргумент зберігає регістр).
    char* arg = line;
    while (*arg != '\0' && *arg != ' ') {
        *arg = static_cast<char>(tolower(static_cast<unsigned char>(*arg)));
        ++arg;
    }
    if (*arg != '\0') {
        *arg++ = '\0';
        while (*arg == ' ') ++arg;
    }
    const char* cmd = line;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0 || strcmp(cmd, "?") == 0) {
        printHelp();
    } else if (strcmp(cmd, "play") == 0) {
        cmdPlay(arg);
    } else if (strcmp(cmd, "stop") == 0) {
        Serial.printf("[PTEST] stop -> %s\n", AudioPlayer::stop() ? "queued" : "REJECTED");
    } else if (strcmp(cmd, "info") == 0) {
        printInfo();
    } else if (strcmp(cmd, "norm") == 0) {
        cmdNorm(arg);
    } else if (strcmp(cmd, "vol") == 0) {
        cmdVol(arg);
    } else if (strcmp(cmd, "pmute") == 0) {
        cmdPmute(arg);
    } else {
        Serial.printf("[PTEST] unknown command \"%s\" (try `help`)\n", cmd);
    }
}

void testTask(void*) {
    char line[player_cfg::kTestLineMax];
    size_t len = 0;
    bool overflow = false;

    for (;;) {
        while (Serial.available() > 0) {
            const int c = Serial.read();
            if (c < 0) break;
            if (c == '\n' || c == '\r') {
                if (overflow) {
                    Serial.println("[PTEST] line too long, ignored");
                } else if (len > 0) {
                    line[len] = '\0';
                    execute(line);
                }
                len = 0;
                overflow = false;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = static_cast<char>(c);
            } else {
                overflow = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(player_cfg::kTestPollMs));
    }
}

}  // namespace

bool AudioPlayerTest::begin(AudioProcessor* processorOrNull) {
    if (s_task != nullptr) {
        return true;
    }
    s_proc = processorOrNull;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        testTask, "player_test", player_cfg::kTestTaskStackBytes, nullptr,
        player_cfg::kTestTaskPriority, &s_task, player_cfg::kTestTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        return false;
    }
    return true;
}

#endif  // AUDIO_PLAYER_TEST
