#include "usb/usb_now_playing.h"

#include <Arduino.h>
#include <string.h>

#include "audio/audio_player.h"
#include "config/features.h"
#include "config/file_player_config.h"
#include "config/usb_now_playing_config.h"
#include "usb/usb_np_text.h"

#if FEATURE_USB_MODE
#include "usb/usb_mode.h"
#include "usb/usb_queue.h"
#endif

static_assert(usb_now_playing_cfg::kTextMax == file_player_cfg::kTagMaxBytes,
              "now-playing text buffers must match the ID3 tag buffers (file_player_cfg::kTagMaxBytes)");

namespace usb_now_playing {

namespace {

#if USB_NOWPLAYING_TRACE
UsbNowPlaying s_last;
bool s_haveLast = false;
#if FEATURE_USB_MODE
uint32_t s_lockFailCount = 0;
uint32_t s_lockFailLogMs = 0;
#endif

// Лише при зміні даних (у т. ч. секунди часу): ≈ 1 рядок / с, поки грає трек.
void trace(const UsbNowPlaying& n) {
    if (s_haveLast && memcmp(&n, &s_last, sizeof(n)) == 0) return;   // без padding: порівняння байтове
    s_last = n;
    s_haveLast = true;
    Serial.printf("[USBNP] act=%u pause=%u pos=%u/%u shuf=%u rep=%u t=%lu/%lu title='%s' artist='%s'\n",
                  static_cast<unsigned>(n.active), static_cast<unsigned>(n.paused), static_cast<unsigned>(n.pos),
                  static_cast<unsigned>(n.count), static_cast<unsigned>(n.shuffle),
                  static_cast<unsigned>(n.repeatAll), static_cast<unsigned long>(n.posSec),
                  static_cast<unsigned long>(n.durSec), n.title, n.artist);
}
#endif

#if FEATURE_USB_MODE
// Статичні буфери: collect() викликається з однієї задачі (див. заголовок), стек контролера не навантажуємо.
char s_path[file_player_cfg::kPathMax];
AudioPlayer::FileTags s_tags;
#endif

}  // namespace

bool collect(UsbNowPlaying* out) {
    if (out == nullptr) return false;
    memset(out, 0, sizeof(*out));   // усі поля й хвости буферів нульові (padding-а нема: static_assert в app_state.h)

#if FEATURE_USB_MODE
    const usb_mode::State ms = usb_mode::state();
    if (ms == usb_mode::State::Ready || ms == usb_mode::State::Playing) {
        usb_queue::Status qs;
        if (!usb_queue::status(&qs)) {
#if USB_NOWPLAYING_TRACE
            ++s_lockFailCount;
            const uint32_t now = millis();
            if (now - s_lockFailLogMs >= usb_now_playing_cfg::kLockFailLogMs) {
                s_lockFailLogMs = now;
                Serial.printf("[USBNP] queue lock busy (count=%lu), keeping previous data\n",
                              static_cast<unsigned long>(s_lockFailCount));
            }
#endif
            return false;
        }
        out->count = qs.count;
        out->shuffle = qs.shuffle;
        out->repeatAll = qs.repeatAll;

        if (qs.state != usb_queue::State::Idle) {   // Starting / Playing (на паузі черга лишається Playing)
            out->active = true;
            out->paused = qs.paused;
            out->pos = (qs.pos > 0 && qs.pos <= 65535) ? static_cast<uint16_t>(qs.pos) : 0;
            out->posSec = AudioPlayer::filePositionSec();
            out->durSec = AudioPlayer::fileDurationSec();

            // Шлях останнього запитаного файла = той самий рядок, що передано в playFile (за ним рахується хеш тегів).
            const bool havePath = AudioPlayer::currentFilePath(s_path, sizeof(s_path));
            if (havePath && AudioPlayer::fileTags(s_path, &s_tags)) {
                usb_np_text::sanitize(s_tags.title, out->title, sizeof(out->title));
                usb_np_text::sanitize(s_tags.artist, out->artist, sizeof(out->artist));
            }
            if (out->title[0] == '\0' && havePath) {   // тегу нема / порожній після нормалізації -> імʼя файла без розширення
                usb_np_text::displayNameFromPath(s_path, out->title, sizeof(out->title));
            }
        }
    }
#endif

#if USB_NOWPLAYING_TRACE
    trace(*out);
#endif
    return true;
}

}  // namespace usb_now_playing
