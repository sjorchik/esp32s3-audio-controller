// [USB7] Консоль відтворення файлу з /usb: usb.play / usb.pstat / usb.seek / usb.stopf / usb.ended.
// Стиль — як у usb_msc_mount.cpp / usb_fs.cpp (тег «[USB]», відлуння «got:» друкує usb_msc_mount::handleLine).

#include "usb/usb_play_diag.h"

#if FEATURE_USB_HOST && USB_HOST_DIAG

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_player.h"
#include "config/file_player_config.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_fs.h"
#include "usb/usb_msc_mount.h"
#if FEATURE_USB_MODE
#include "usb/usb_mode.h"  // [USB9] usb.mode … (диспетчеризація тут: usb_msc_mount.cpp не змінюється)
#endif

namespace usb_play_diag {

namespace {

constexpr const char* kTag = "[USB]";

// Слово команди: рядок починається з word і далі пробіл або кінець; *rest — хвіст після пробілів.
bool matchWord(const char* s, const char* word, const char** rest) {
    const size_t n = strlen(word);
    if (strncmp(s, word, n) != 0 || (s[n] != '\0' && s[n] != ' ' && s[n] != '\t')) {
        return false;
    }
    const char* r = s + n;
    while (*r == ' ' || *r == '\t') {
        ++r;
    }
    *rest = r;
    return true;
}

const char* const kWords[] = {"usb.play", "usb.pstat", "usb.seek", "usb.stopf", "usb.ended", "usb.fx"
#if FEATURE_USB_MODE
                                 ,
                                 "usb.mode"  // [USB9]
#endif
};

void printHeap(const char* label) {
    Serial.printf("%s heap[%s] internal free=%u largest=%u min_free=%u psram_free=%u\n", kTag, label,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// IDF: uxTaskGetStackHighWaterMark повертає БАЙТИ (мінімум вільного стека від створення задачі, кумулятивний).
void printStack(const char* name, const char* taskName) {
    const TaskHandle_t h = xTaskGetHandle(taskName);
    if (h == nullptr) {
        Serial.printf("%s pstat: stack %s (task '%s'): task not found\n", kTag, name, taskName);
        return;
    }
    Serial.printf("%s pstat: stack %s (task '%s'): min free=%u bytes (since boot)\n", kTag, name, taskName,
                  (unsigned)uxTaskGetStackHighWaterMark(h));
}

// usb.play <шлях> [старт]. Шлях може містити пробіли (кирилиця, «Папка 1/пісня.mp3»): шлях — весь хвіст, окрім
// останнього слова з одних цифр (воно — стартова секунда). Ім'я файлу, що закінчується на « <цифри>», так не задати
// (те саме обмеження, що й у usb.fs.read).
void cmdPlay(const char* rest) {
    char path[file_player_cfg::kPathMax];
    if (rest == nullptr || rest[0] == '\0' || strlen(rest) >= sizeof(path)) {
        Serial.printf("%s usage: usb.play <path> [start_sec]   (path from the volume root, e.g. /song.mp3)\n", kTag);
        return;
    }
    strcpy(path, rest);
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == ' ' || path[n - 1] == '\t')) {
        path[--n] = '\0';
    }
    int32_t start = -1;
    size_t sp = n;
    while (sp > 0 && path[sp - 1] != ' ' && path[sp - 1] != '\t') {
        --sp;
    }
    if (sp > 0 && sp < n) {
        bool digits = true;
        for (size_t i = sp; i < n; ++i) {
            if (path[i] < '0' || path[i] > '9') {
                digits = false;
                break;
            }
        }
        if (digits && (n - sp) <= 6) {
            start = static_cast<int32_t>(atol(path + sp));
            size_t cut = sp;
            while (cut > 0 && (path[cut - 1] == ' ' || path[cut - 1] == '\t')) {
                --cut;
            }
            path[cut] = '\0';
        }
    }
    if (path[0] == '\0') {
        Serial.printf("%s usage: usb.play <path> [start_sec]\n", kTag);
        return;
    }
    printHeap("before play");
    usb_fs::resetReadStats();
    const bool ok = AudioPlayer::playFile(path, start);
    Serial.printf("%s play '%s' start=%ld: %s\n", kTag, path, static_cast<long>(start),
                  ok ? "request accepted (watch [PLAYER] lines / usb.pstat)" : "REFUSED (see [PLAYER] line above)");
}

void cmdPstat() {
    char path[file_player_cfg::kPathMax];
    const bool havePath = AudioPlayer::currentFilePath(path, sizeof(path));
    const char* err = AudioPlayer::lastFileError();
    Serial.printf("%s pstat: usb=%s mounted=%d | player=%s source=%s file_active=%d dac_unmuted=%d trim=%d dB\n", kTag,
                  usb_msc_mount::stateName(usb_msc_mount::state()), (int)usb_msc_mount::isMounted(),
                  AudioPlayer::stateName(AudioPlayer::state()), AudioPlayer::isFileSource() ? "file" : "radio",
                  (int)AudioPlayer::fileActive(), (int)AudioPlayer::dacUnmuted(), (int)AudioPlayer::outputTrimDb());
    Serial.printf("%s pstat: file='%s' pos=%lu s dur=%lu s ended_pending=%d last_error=%s reconnects=%lu\n", kTag,
                  havePath ? path : "-", (unsigned long)AudioPlayer::filePositionSec(),
                  (unsigned long)AudioPlayer::fileDurationSec(), (int)AudioPlayer::fileEndedPending(),
                  err != nullptr ? err : "-", (unsigned long)AudioPlayer::reconnectCount());
    printStack("decoder", file_player_cfg::kDecoderTaskName);
    printStack("player", file_player_cfg::kPlayerTaskName);
    printHeap("pstat");
    const usb_fs::ReadStats r = usb_fs::readStats();
    Serial.printf("%s pstat: usb_fs reads=%lu max=%lu.%03lu ms slow(>=%lu ms)=%lu bytes=%lu\n", kTag,
                  (unsigned long)r.count, (unsigned long)(r.maxUs / 1000), (unsigned long)(r.maxUs % 1000),
                  (unsigned long)(usb_host_cfg::kFsSlowReadUs / 1000), (unsigned long)r.slowCount,
                  (unsigned long)r.bytes);
}

// usb.seek <секунди> (зі знаком). Друкує позицію до / після й обидві інтерпретації очікуваної (відносну й абсолютну),
// бо семантика Audio::setTimeOffset у закріпленому коміті не звірена.
void cmdSeek(const char* rest) {
    if (rest == nullptr || rest[0] == '\0') {
        Serial.printf("%s usage: usb.seek <seconds>   (signed; passed to Audio::setTimeOffset)\n", kTag);
        return;
    }
    if (!AudioPlayer::fileActive()) {
        Serial.printf("%s seek: no file is playing\n", kTag);
        return;
    }
    const long sec = atol(rest);
    const unsigned long before = AudioPlayer::filePositionSec();
    const unsigned long dur = AudioPlayer::fileDurationSec();
    if (!AudioPlayer::seekFileSec(static_cast<int32_t>(sec))) {
        Serial.printf("%s seek: refused\n", kTag);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(file_player_cfg::kSeekSettleMs));
    const unsigned long after = AudioPlayer::filePositionSec();
    Serial.printf("%s seek: arg=%ld before=%lu s after=%lu s dur=%lu s | expected if RELATIVE=%ld, if ABSOLUTE=%ld "
                  "(+~%lu s elapsed during the wait)\n",
                  kTag, sec, before, after, dur, (long)before + sec, sec,
                  (unsigned long)(file_player_cfg::kSeekSettleMs / 1000));
}

void cmdStopf() {
    if (!AudioPlayer::fileActive()) {
        Serial.printf("%s stopf: no file is playing (player=%s source=%s) - nothing done\n", kTag,
                      AudioPlayer::stateName(AudioPlayer::state()), AudioPlayer::isFileSource() ? "file" : "radio");
        return;
    }
    Serial.printf("%s stopf: stop requested: %s\n", kTag, AudioPlayer::stop() ? "accepted" : "REFUSED");
}

void cmdEnded(const char* rest) {
    const bool take = (rest != nullptr && strcmp(rest, "take") == 0);
    const bool pending = take ? AudioPlayer::takeFileEnded() : AudioPlayer::fileEndedPending();
    const uint32_t at = AudioPlayer::fileEndedAtMs();
    if (at != 0) {
        Serial.printf("%s ended: %s=%d (raised at t=%lu ms, %lu ms ago)\n", kTag, take ? "taken" : "pending",
                      (int)pending, (unsigned long)at, (unsigned long)(millis() - at));
    } else {
        Serial.printf("%s ended: %s=%d (never raised since the last file start)\n", kTag, take ? "taken" : "pending",
                      (int)pending);
    }
}

// usb.fx [extra <мс> | dig <мс|-1> | level <дБ>]: без аргументів — показати. Діє на НАСТУПНИЙ usb.play.
void cmdFx(const char* rest) {
    uint32_t extra = 0;
    int32_t dig = 0;
    int level = 0;
    AudioPlayer::getFileTune(&extra, &dig, &level);
    if (rest != nullptr && rest[0] != '\0') {
        const char* arg = nullptr;
        if (matchWord(rest, "extra", &arg) && arg[0] != '\0') {
            const long v = atol(arg);
            extra = (v < 0) ? 0u : static_cast<uint32_t>(v);
        } else if (matchWord(rest, "dig", &arg) && arg[0] != '\0') {
            dig = static_cast<int32_t>(atol(arg));
        } else if (matchWord(rest, "level", &arg) && arg[0] != '\0') {
            level = static_cast<int>(atol(arg));
        } else {
            Serial.printf("%s usage: usb.fx [extra <ms> | dig <ms|-1> | level <dB>]\n", kTag);
            return;
        }
        AudioPlayer::setFileTune(extra, dig, level);
        AudioPlayer::getFileTune(&extra, &dig, &level);
    }
    Serial.printf("%s fx: extra_settle=%lu ms dig_delay=%ld ms%s level=%d dB (applies to the NEXT usb.play)\n", kTag,
                  (unsigned long)extra, (long)dig, dig < 0 ? " (radio DSP unmuted at file start)" : "", level);
}

}  // namespace

bool isPlayCommand(const char* line) {
    if (line == nullptr) {
        return false;
    }
    const char* rest = nullptr;
    for (const char* w : kWords) {
        if (matchWord(line, w, &rest)) {
            return true;
        }
    }
    return false;
}

void handleConsoleLine(const char* line) {
    const char* rest = nullptr;
    if (matchWord(line, "usb.play", &rest)) {
        cmdPlay(rest);
    } else if (matchWord(line, "usb.pstat", &rest)) {
        cmdPstat();
    } else if (matchWord(line, "usb.seek", &rest)) {
        cmdSeek(rest);
    } else if (matchWord(line, "usb.stopf", &rest)) {
        cmdStopf();
    } else if (matchWord(line, "usb.ended", &rest)) {
        cmdEnded(rest);
    } else if (matchWord(line, "usb.fx", &rest)) {
        cmdFx(rest);
#if FEATURE_USB_MODE
    } else if (matchWord(line, "usb.mode", &rest)) {
        usb_mode::handleConsoleLine(line);  // [USB9]
#endif
    } else {
        Serial.printf("%s play: unknown command\n", kTag);
    }
}

}  // namespace usb_play_diag

#endif  // FEATURE_USB_HOST && USB_HOST_DIAG
