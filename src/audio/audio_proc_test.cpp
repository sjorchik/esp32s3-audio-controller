#include "audio/audio_proc_test.h"

#if AUDIO_PROC_TEST

#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_processor.h"
#include "config/audio_config.h"

// Протокол (рядок команди закінчується Enter; регістр літер не важливий):
//   i<N>    вхід 0..N-1              (N з capabilities().inputCount)
//   v<N>    гучність 0..100
//   b<N>    бас  -7..+7   (крок 2 дБ)
//   t<N>    дискант -7..+7 (крок 2 дБ)
//   a<N>    баланс -20..+20 (крок 1.25 дБ; + = правий гучніший)
//   m       мʼют: перемкнути;  m1 = увімкнути, m0 = вимкнути
//   l       loudness: перемкнути; l1 / l0 (лише PT2313L 28-pin)
//   p       надрукувати capabilities() і поточний кеш
//   r       applyAll() — повторно надіслати весь стан
//   x       probe() — чи є ACK на адресі 0x44
//   h / ?   допомога
// Відповідь на кожну команду: "[ATEST] <що> -> OK|FAIL".

namespace {

AudioProcessor* s_proc = nullptr;
TaskHandle_t s_task = nullptr;

void printHelp() {
    Serial.println("[ATEST] commands (end with Enter):");
    Serial.println("[ATEST]   i<N> input   v<N> volume   b<N> bass   t<N> treble   a<N> balance");
    Serial.println("[ATEST]   m[0|1] mute  l[0|1] loudness   p print   r reapply   x probe   h help");
    Serial.printf("[ATEST] ranges: v %d..%d, b/t %d..%d (x%d dB), a %d..%d (+ = right louder)\n",
                  audio_cfg::kVolumeUiMin, audio_cfg::kVolumeUiMax,
                  audio_cfg::kToneUiMin, audio_cfg::kToneUiMax, audio_cfg::kToneStepDb,
                  audio_cfg::kBalanceUiMin, audio_cfg::kBalanceUiMax);
}

void printInfo() {
    const AudioProcessorCapabilities c = s_proc->capabilities();
    Serial.printf("[ATEST] caps: bass=%d treble=%d balance=%d loudness=%d fader=%d "
                  "inputGain=%d inputs=%u\n",
                  c.bass, c.treble, c.balance, c.loudness, c.fader, c.inputGain,
                  static_cast<unsigned>(c.inputCount));
    Serial.printf("[ATEST] range: vol=%d..%d tone=%d..%d bal=%d..%d\n",
                  c.volumeMin, c.volumeMax, c.toneMin, c.toneMax, c.balanceMin, c.balanceMax);
    const AudioProcessorState s = s_proc->cachedState();
    Serial.printf("[ATEST] state: input=%u vol=%d bass=%d treble=%d bal=%d mute=%d loud=%d "
                  "i2cErrors=%lu\n",
                  static_cast<unsigned>(s.input), s.volume, s.bass, s.treble, s.balance,
                  s.mute, s.loudness, static_cast<unsigned long>(s_proc->i2cErrorCount()));
}

bool parseLong(const char* s, long& out) {
    char* end = nullptr;
    out = strtol(s, &end, 10);
    return end != s && *end == '\0';
}

int8_t toI8(long v) {
    if (v < -128) return -128;
    if (v > 127) return 127;
    return static_cast<int8_t>(v);
}

void report(const char* what, long value, bool ok) {
    Serial.printf("[ATEST] %s %ld -> %s\n", what, value, ok ? "OK" : "FAIL");
}

void handleLine(char* line) {
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    // Пробіли в кінці (деякі монітори їх додають) відкидаємо.
    char* end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    if (*line == '\0') {
        return;
    }
    const char cmd = static_cast<char>(tolower(static_cast<unsigned char>(*line)));
    const char* arg = line + 1;
    while (*arg == ' ') {
        ++arg;
    }
    const bool hasArg = (*arg != '\0');
    long val = 0;
    if (hasArg && !parseLong(arg, val)) {
        Serial.printf("[ATEST] bad argument: '%s'\n", arg);
        return;
    }

    switch (cmd) {
        case 'i':
        case 'v':
        case 'b':
        case 't':
        case 'a':
            if (!hasArg) {
                Serial.printf("[ATEST] '%c' needs a number, e.g. %c5\n", cmd, cmd);
                return;
            }
            break;
        default:
            break;
    }

    switch (cmd) {
        case 'i':
            if (val < 0 || val > 255) {
                Serial.println("[ATEST] input index out of range");
                return;
            }
            report("input", val, s_proc->setInput(static_cast<uint8_t>(val)));
            break;
        case 'v':
            report("volume", val, s_proc->setVolume(toI8(val)));
            break;
        case 'b':
            report("bass", val, s_proc->setBass(toI8(val)));
            break;
        case 't':
            report("treble", val, s_proc->setTreble(toI8(val)));
            break;
        case 'a':
            report("balance", val, s_proc->setBalance(toI8(val)));
            break;
        case 'm': {
            const bool target = hasArg ? (val != 0) : !s_proc->cachedState().mute;
            report("mute", target ? 1 : 0, s_proc->setMute(target));
            break;
        }
        case 'l': {
            const bool target = hasArg ? (val != 0) : !s_proc->cachedState().loudness;
            report("loudness", target ? 1 : 0, s_proc->setLoudness(target));
            break;
        }
        case 'p':
            printInfo();
            break;
        case 'r':
            Serial.printf("[ATEST] applyAll -> %s\n", s_proc->applyAll() ? "OK" : "FAIL");
            break;
        case 'x':
            Serial.printf("[ATEST] probe -> %s\n", s_proc->probe() ? "ACK" : "no ACK");
            break;
        case 'h':
        case '?':
            printHelp();
            break;
        default:
            Serial.printf("[ATEST] unknown command '%c' (h = help)\n", cmd);
            break;
    }
}

void taskMain(void*) {
    char line[audio_cfg::kTestLineMax];
    size_t len = 0;
    bool overflow = false;

    for (;;) {
        while (Serial.available() > 0) {
            const int c = Serial.read();
            if (c < 0) {
                break;
            }
            if (c == '\n' || c == '\r') {
                if (overflow) {
                    Serial.println("[ATEST] line too long, ignored");
                } else if (len > 0) {
                    line[len] = '\0';
                    handleLine(line);
                }
                len = 0;
                overflow = false;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = static_cast<char>(c);
            } else {
                overflow = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(audio_cfg::kTestPollMs));
    }
}

}  // namespace

bool AudioProcTest::begin(AudioProcessor* proc) {
    if (proc == nullptr) {
        return false;
    }
    if (s_task != nullptr) {
        return true;
    }
    s_proc = proc;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        taskMain, "audio_test", audio_cfg::kTestTaskStackBytes, nullptr,
        audio_cfg::kTestTaskPriority, &s_task, audio_cfg::kTestTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        return false;
    }
    printHelp();
    return true;
}

#endif  // AUDIO_PROC_TEST
