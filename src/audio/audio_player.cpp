// Аудіоплеєр інтернет-радіо (ESP32-audioI2S).
//
// ЗАЛЕЖНІСТЬ ВІД ЗОВНІШНЬОЇ БІБЛІОТЕКИ: у файлі використано такі елементи API
// ESP32-audioI2S (їх треба звірити з Audio.h вашої версії при першій збірці):
//   Audio(), setPinout(), setBufsize() [лише при AUDIO_PLAYER_HAS_SETBUFSIZE],
//   setConnectionTimeout(), setVolume(), connecttohost(), stopSong(), loop(),
//   isRunning(), inBufferFilled(), inBufferFree(), getSampleRate() [P30, лише при
//   EQ_SAMPLE_RATE_FROM_LIB], а також callback-и (стиль обирає AUDIO_PLAYER_CB_STYLE):
//   стиль 1: audio_info, audio_showstation, audio_showstreamtitle, audio_bitrate,
//            audio_eof_stream, audio_id3data;
//   стиль 2: Audio::audio_info_callback, Audio::msg_t, Audio::evt_info,
//            evt_bitrate, evt_eof, evt_name, evt_streamtitle, evt_id3data;

#include "audio/audio_player.h"

#include <Audio.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_processor.h"
#include "audio/eq.h"
#include "audio/output_trim.h"
#include "config/audio_player_config.h"
#include "config/eq_config.h"
#include "config/pins.h"

#if AUDIO_PLAYER_DEBUG
#define PLAYER_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define PLAYER_LOG(...) \
    do {                \
    } while (0)
#endif

namespace {

// ===========================================================================
// ICY-текст: визначення кодування, перекодування в UTF-8, нормалізація
// ===========================================================================
// ==== ICY-BEGIN

enum class IcyEncoding : uint8_t { Ascii, Utf8, Cp1251, Latin1 };

// Windows-1251, байти 0x80..0xBF -> Unicode (0xFFFD = не визначено).
// 0xC0..0xFF: U+0410 + (b - 0xC0), тобто А..я.
constexpr uint16_t kCp1251Table[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

bool isAsciiLetter(uint8_t b) {
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z');
}

// Декодує одну UTF-8 послідовність з позиції i. Повертає її довжину або 0,
// якщо послідовність некоректна (обірвана, overlong, сурогат, > U+10FFFF).
size_t utf8Decode(const uint8_t* s, size_t n, size_t i, uint32_t& cp) {
    const uint8_t b0 = s[i];
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    }
    size_t len = 0;
    uint32_t minCp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        len = 2;
        cp = b0 & 0x1F;
        minCp = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        len = 3;
        cp = b0 & 0x0F;
        minCp = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        len = 4;
        cp = b0 & 0x07;
        minCp = 0x10000;
    } else {
        return 0;
    }
    if (i + len > n) {
        return 0;
    }
    for (size_t k = 1; k < len; ++k) {
        const uint8_t b = s[i + k];
        if ((b & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return len;
}

// Евристика (це НЕ точна наука):
//  1. усі байти < 0x80                          -> ASCII;
//  2. вся послідовність — коректний UTF-8       -> UTF-8 (випадкова
//     кирилиця cp1251 практично ніколи не є валідним UTF-8);
//  3. інакше розрізняємо cp1251 і Latin-1 за байтами 0xC0..0xFF (у cp1251 це
//     літери А..я, у Latin-1 — А-ÿ):
//       - байт у «пробігу» (сусід теж >= 0xC0)  -> голос за cp1251;
//       - одиночний байт з ASCII-літерою поруч  -> голос за Latin-1
//         («Café», «Motörhead»);
//       - одиночний байт між пробілами/розділовими (однолітерне слово)
//         -> голос за cp1251;
//     при рівності перемагає cp1251 (проєкт орієнтований на українські станції);
//  4. якщо байтів >= 0xC0 нема, а є 0x80..0x9F (типографські тире/лапки, які
//     мають однакові коди в cp1251 і cp1252) -> cp1251; інакше Latin-1.
IcyEncoding detectEncoding(const uint8_t* s, size_t n) {
    bool high = false;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] >= 0x80) {
            high = true;
            break;
        }
    }
    if (!high) {
        return IcyEncoding::Ascii;
    }

    bool validUtf8 = true;
    for (size_t i = 0; i < n;) {
        uint32_t cp = 0;
        const size_t l = utf8Decode(s, n, i, cp);
        if (l == 0) {
            validUtf8 = false;
            break;
        }
        i += l;
    }
    if (validUtf8) {
        return IcyEncoding::Utf8;
    }

    uint32_t cyr = 0;
    uint32_t lat = 0;
    uint32_t c1 = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = s[i];
        if (b < 0xC0) {
            if (b >= 0x80 && b < 0xA0) {
                ++c1;
            }
            continue;
        }
        const bool prevHigh = (i > 0) && s[i - 1] >= 0xC0;
        const bool nextHigh = (i + 1 < n) && s[i + 1] >= 0xC0;
        if (prevHigh || nextHigh) {
            ++cyr;
            continue;
        }
        const bool prevLetter = (i > 0) && isAsciiLetter(s[i - 1]);
        const bool nextLetter = (i + 1 < n) && isAsciiLetter(s[i + 1]);
        if (prevLetter || nextLetter) {
            ++lat;
        } else {
            ++cyr;
        }
    }
    if (cyr == 0 && lat == 0) {
        return (c1 > 0) ? IcyEncoding::Cp1251 : IcyEncoding::Latin1;
    }
    return (cyr >= lat) ? IcyEncoding::Cp1251 : IcyEncoding::Latin1;
}

uint32_t cp1251ToCodepoint(uint8_t b) {
    if (b < 0x80) return b;
    if (b < 0xC0) return kCp1251Table[b - 0x80];
    return 0x0410u + (b - 0xC0u);
}

// Latin-1: байти 0xA0..0xFF = U+00A0..U+00FF. Діапазон 0x80..0x9F у Latin-1 —
// керуючі, але на практиці там Windows-1252; беремо лише поширені типографські.
uint32_t latin1ToCodepoint(uint8_t b) {
    if (b < 0x80 || b >= 0xA0) return b;
    switch (b) {
        case 0x80: return 0x20AC;
        case 0x85: return 0x2026;
        case 0x91: return 0x2018;
        case 0x92: return 0x2019;
        case 0x93: return 0x201C;
        case 0x94: return 0x201D;
        case 0x95: return 0x2022;
        case 0x96: return 0x2013;
        case 0x97: return 0x2014;
        case 0x99: return 0x2122;
        default:   return 0xFFFD;
    }
}

// Покриття шрифта UI (розділ 10 MASTER SPEC): ASCII, Latin-1, Latin Ext-A,
// кирилиця U+0400..045F, Ґ ґ. Має збігатися з ui/font_data.h — звірити.
bool glyphAvailable(uint32_t cp) {
    return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF) ||
           (cp >= 0x100 && cp <= 0x17F) || (cp >= 0x400 && cp <= 0x45F) ||
           cp == 0x490 || cp == 0x491;
}

// ASCII-аналоги типографських символів, яких немає у шрифті. nullptr — немає.
const char* typographicAscii(uint32_t cp) {
    switch (cp) {
        case 0x2010: case 0x2011: case 0x2012:
        case 0x2013: case 0x2014: case 0x2015: case 0x2212:
            return "-";
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
            return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
            return "\"";
        case 0x2026: return "...";
        case 0x2022: return "*";
        case 0x2116: return "No";
        case 0x2122: return "TM";
        case 0x20AC: return "EUR";
        default:     return nullptr;
    }
}

struct Out {
    char* buf;
    size_t cap;
    size_t len;
};

bool putBytes(Out& o, const char* s, size_t l) {
    if (o.len + l >= o.cap) {  // лишаємо місце для '\0'
        return false;
    }
    memcpy(o.buf + o.len, s, l);
    o.len += l;
    return true;
}

bool putCodepoint(Out& o, uint32_t cp) {
    char t[4];
    size_t l = 0;
    if (cp < 0x80) {
        t[l++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        t[l++] = static_cast<char>(0xC0 | (cp >> 6));
        t[l++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        t[l++] = static_cast<char>(0xE0 | (cp >> 12));
        t[l++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        t[l++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        t[l++] = static_cast<char>(0xF0 | (cp >> 18));
        t[l++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        t[l++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        t[l++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    return putBytes(o, t, l);
}

// false — вихідний буфер повний (все або нічого для одного символу).
bool emitCodepoint(Out& o, uint32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0) {
        cp = ' ';
    }
    if (cp < 0x20 || cp == 0x7F || cp == 0xAD) {
        return true;  // керуючі та мʼякий перенос відкидаємо
    }
    if (cp == ' ' && (o.len == 0 || o.buf[o.len - 1] == ' ')) {
        return true;  // без початкових і подвійних пробілів
    }
    if (player_cfg::kNormalizeForFont) {
        const char* rep = typographicAscii(cp);
        if (rep != nullptr) {
            return putBytes(o, rep, strlen(rep));
        }
        if (!glyphAvailable(cp)) {
            return putBytes(o, "?", 1);
        }
    }
    return putCodepoint(o, cp);
}

const char* icyToUtf8(const char* in, char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return "none";
    }
    out[0] = '\0';
    if (in == nullptr) {
        return "none";
    }
    const size_t n = strnlen(in, player_cfg::kIcyRawMax);
    const uint8_t* s = reinterpret_cast<const uint8_t*>(in);
    const IcyEncoding enc = detectEncoding(s, n);

    Out o{out, cap, 0};
    for (size_t i = 0; i < n;) {
        uint32_t cp = 0xFFFD;
        size_t adv = 1;
        switch (enc) {
            case IcyEncoding::Ascii:
                cp = s[i];
                break;
            case IcyEncoding::Utf8:
                adv = utf8Decode(s, n, i, cp);
                if (adv == 0) {
                    cp = 0xFFFD;
                    adv = 1;
                }
                break;
            case IcyEncoding::Cp1251:
                cp = cp1251ToCodepoint(s[i]);
                break;
            case IcyEncoding::Latin1:
                cp = latin1ToCodepoint(s[i]);
                break;
        }
        i += adv;
        if (!emitCodepoint(o, cp)) {
            break;
        }
    }
    while (o.len > 0 && out[o.len - 1] == ' ') {
        --o.len;
    }
    out[o.len] = '\0';

    switch (enc) {
        case IcyEncoding::Ascii:  return "ascii";
        case IcyEncoding::Utf8:   return "utf8";
        case IcyEncoding::Cp1251: return "cp1251";
        case IcyEncoding::Latin1: return "latin1";
    }
    return "none";
}

// Копія рядка з обрізанням по межі UTF-8-символу.
void copyUtf8Trunc(char* dst, size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) {
            --n;  // src[n] — продовження: відступаємо до початку символу
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// ==== ICY-END

// ===========================================================================
// Стан плеєра
// ===========================================================================

enum class Cmd : uint8_t { None, Play, Stop };

Audio* s_audio = nullptr;
AudioProcessor* s_proc = nullptr;  // не володіємо; setMute() не викликаємо
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;

// Під мʼютексом: команди й метадані.
Cmd s_cmd = Cmd::None;
char s_pendingUrl[player_cfg::kUrlMax];
char s_station[player_cfg::kStationMax];
char s_title[player_cfg::kTitleMax];

// Пише лише задача плеєра (читаються з інших задач як volatile).
volatile PlayerState s_state = PlayerState::Idle;
volatile uint32_t s_reconnectCount = 0;
volatile uint32_t s_underruns = 0;
volatile int s_httpCode = 0;

// Прапорці з callback-ів (callback виконується в контексті Audio::loop()).
volatile bool s_evtEof = false;
volatile bool s_evtBitrate = false;
volatile bool s_evtStreamReady = false;

// Тільки задача плеєра.
char s_url[player_cfg::kUrlMax];
bool s_wantPlaying = false;
bool s_dacMuted = true;
bool s_stableDone = false;    // Playing тривав >= kUnmuteStableMs (мʼют знято)
bool s_rebuffering = false;   // Buffering через просідання буфера, а не старт
bool s_notRunning = false;
uint32_t s_notRunningSinceMs = 0;
uint32_t s_backoffMs = player_cfg::kReconnectInitialMs;
uint32_t s_nextRetryMs = 0;
uint32_t s_stateSinceMs = 0;
uint32_t s_playingSinceMs = 0;
uint32_t s_connectedAtMs = 0;
uint32_t s_lastBufCheckMs = 0;
uint8_t s_bufPercent = 0;

// RAII-захоплення мʼютекса на короткий час.
class Guard {
public:
    Guard()
        : m_ok(s_mutex != nullptr &&
               xSemaphoreTake(s_mutex, pdMS_TO_TICKS(player_cfg::kMutexTimeoutMs)) == pdTRUE) {}
    ~Guard() {
        if (m_ok) {
            xSemaphoreGive(s_mutex);
        }
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    bool ok() const { return m_ok; }

private:
    bool m_ok;
};

bool timeReached(uint32_t now, uint32_t target) {
    return static_cast<int32_t>(now - target) >= 0;
}

TickType_t msToTicksAtLeastOne(uint32_t ms) {
    const TickType_t t = pdMS_TO_TICKS(ms);
    return (t == 0) ? 1 : t;
}

// --- XSMT ---

void muteDac(bool mute) {
    digitalWrite(pins::kXsmt,
                 mute ? player_cfg::kXsmtMutedLevel : player_cfg::kXsmtUnmutedLevel);
    if (mute != s_dacMuted) {
        PLAYER_LOG("[PLAYER] XSMT %s\n", mute ? "mute" : "unmute");
    }
    s_dacMuted = mute;
}

// --- Стан ---

void setState(PlayerState ns, const char* why) {
    const PlayerState old = s_state;
    if (old == ns) {
        return;
    }
    s_state = ns;
    s_stateSinceMs = millis();
    if (ns == PlayerState::Playing) {
        s_playingSinceMs = s_stateSinceMs;
    }
    PLAYER_LOG("[PLAYER] state %s -> %s%s%s\n", AudioPlayer::stateName(old),
               AudioPlayer::stateName(ns), (why != nullptr) ? ": " : "",
               (why != nullptr) ? why : "");
}

void clearMetadata() {
    Guard g;
    if (g.ok()) {
        s_station[0] = '\0';
        s_title[0] = '\0';
    }
}

// --- Обробники подій бібліотеки (спільні для обох стилів callback-ів) ---

void parseHttpCode(const char* text) {
    const char* h = strstr(text, "HTTP/");
    if (h == nullptr) {
        return;
    }
    const char* sp = strchr(h, ' ');
    if (sp == nullptr) {
        return;
    }
    const int code = atoi(sp + 1);
    if (code >= 100 && code < 600) {
        s_httpCode = code;
    }
}

void handleInfo(const char* text) {
    if (text == nullptr) {
        return;
    }
#if AUDIO_PLAYER_LOG_LIBRARY
    Serial.printf("[PLAYER][lib] %s\n", text);
#endif
    parseHttpCode(text);
    if (strstr(text, player_cfg::kStreamReadyText) != nullptr) {
        s_evtStreamReady = true;
    }
}

void handleStation(const char* text) {
    if (text == nullptr) {
        return;
    }
    char tmp[player_cfg::kStationMax];
    const char* enc = icyToUtf8(text, tmp, sizeof(tmp));
    {
        Guard g;
        if (g.ok()) {
            strcpy(s_station, tmp);
        }
    }
    PLAYER_LOG("[PLAYER] station (%s): %s\n", enc, tmp);
}

void handleTitle(const char* text) {
    if (text == nullptr) {
        return;
    }
    char tmp[player_cfg::kTitleMax];
    const char* enc = icyToUtf8(text, tmp, sizeof(tmp));
    {
        Guard g;
        if (g.ok()) {
            strcpy(s_title, tmp);
        }
    }
    PLAYER_LOG("[PLAYER] title (%s): %s\n", enc, tmp);
}

void handleBitrate(const char* text) {
    s_evtBitrate = true;
    PLAYER_LOG("[PLAYER] bitrate: %s\n", (text != nullptr) ? text : "?");
}

void handleEof(const char* text) {
    s_evtEof = true;
    PLAYER_LOG("[PLAYER] eof: %s\n", (text != nullptr) ? text : "?");
}

void handleId3(const char* text) {
    PLAYER_LOG("[PLAYER] id3: %s\n", (text != nullptr) ? text : "?");
}

// --- Підключення й збої ---

// Планує наступну спробу з exponential backoff, мʼютить ЦАП, зупиняє потік.
void failStream(const char* reason) {
    muteDac(true);
    if (s_audio != nullptr) {
        s_audio->stopSong();
    }
    s_stableDone = false;
    s_rebuffering = false;
    s_notRunning = false;
    s_nextRetryMs = millis() + s_backoffMs;
    PLAYER_LOG("[PLAYER] failure: %s; retry in %lu ms (reconnects so far: %lu)\n", reason,
               static_cast<unsigned long>(s_backoffMs),
               static_cast<unsigned long>(s_reconnectCount));
    setState(PlayerState::Error, reason);

    uint64_t next = static_cast<uint64_t>(s_backoffMs) * player_cfg::kReconnectFactor;
    if (next > player_cfg::kReconnectMaxMs) {
        next = player_cfg::kReconnectMaxMs;
    }
    s_backoffMs = static_cast<uint32_t>(next);
}

// Виклик connecttohost() БЛОКУЄ задачу плеєра (DNS/TCP/заголовки) до
// kHttpTimeoutMs/kHttpsTimeoutMs; це задача ядра 1, інші задачі не страждають.
void connectNow(bool isReconnect) {
    setState(isReconnect ? PlayerState::Reconnecting : PlayerState::Connecting, s_url);
    muteDac(true);
    s_stableDone = false;
    s_rebuffering = false;
    s_notRunning = false;
    s_evtEof = false;
    s_evtBitrate = false;
    s_evtStreamReady = false;
    s_bufPercent = 0;

    const bool ok = s_audio->connecttohost(s_url);
    s_connectedAtMs = millis();
    if (!ok) {
        failStream("connecttohost failed");
        return;
    }
    setState(PlayerState::Buffering, "connected");
}

void doPlay(const char* url) {
    s_audio->stopSong();  // безпечно, якщо нічого не грало
    muteDac(true);
    strcpy(s_url, url);  // довжину перевірено в playUrl()
    s_wantPlaying = true;
    s_backoffMs = player_cfg::kReconnectInitialMs;
    s_httpCode = 0;
    clearMetadata();
    PLAYER_LOG("[PLAYER] play: %s\n", s_url);
    connectNow(false);
}

void doStop() {
    s_wantPlaying = false;
    s_audio->stopSong();
    muteDac(true);
    s_stableDone = false;
    s_rebuffering = false;
    s_notRunning = false;
    clearMetadata();
    PLAYER_LOG("[PLAYER] stop\n");
    setState(PlayerState::Idle, "stop");
}

void processCommands() {
    Cmd cmd = Cmd::None;
    char url[player_cfg::kUrlMax];
    url[0] = '\0';
    {
        Guard g;
        if (!g.ok()) {
            return;
        }
        cmd = s_cmd;
        s_cmd = Cmd::None;
        if (cmd == Cmd::Play) {
            memcpy(url, s_pendingUrl, sizeof(url));
        }
    }
    if (cmd == Cmd::Play) {
        doPlay(url);
    } else if (cmd == Cmd::Stop) {
        doStop();
    }
}

uint8_t readBufferPercent() {
    // Загальний розмір = зайняте + вільне (inBufferSize() є не в усіх версіях).
    const uint32_t filled = s_audio->inBufferFilled();
    const uint32_t size = filled + s_audio->inBufferFree();
    if (size == 0) {
        return 0;
    }
    const uint64_t pct = static_cast<uint64_t>(filled) * 100u / size;
    return static_cast<uint8_t>(pct > 100 ? 100 : pct);
}

void monitorStream(uint32_t now) {
    if (s_evtEof) {
        s_evtEof = false;
        failStream("stream ended (eof)");
        return;
    }

    const bool running = s_audio->isRunning();
    if (!running) {
        if (!s_notRunning) {
            s_notRunning = true;
            s_notRunningSinceMs = now;
        } else if (now - s_notRunningSinceMs >= player_cfg::kNotRunningGraceMs) {
            failStream("decoder stopped");
            return;
        }
    } else {
        s_notRunning = false;
    }

    if (now - s_lastBufCheckMs >= player_cfg::kBufferPollMs) {
        s_lastBufCheckMs = now;
        s_bufPercent = readBufferPercent();
    }
    const uint8_t pct = s_bufPercent;

    if (s_state == PlayerState::Buffering) {
        if (s_rebuffering) {
            if (pct >= player_cfg::kBufferRecoverPercent) {
                s_rebuffering = false;
                setState(PlayerState::Playing, "buffer recovered");
            }
        } else {
            const bool ready =
                running &&
                (s_evtBitrate || s_evtStreamReady ||
                 (now - s_connectedAtMs >= player_cfg::kAssumePlayingMs &&
                  pct >= player_cfg::kBufferLowPercent));
            if (ready) {
                setState(PlayerState::Playing, "stream ready");
            }
        }
        if (s_state == PlayerState::Buffering &&
            now - s_stateSinceMs >= player_cfg::kBufferingTimeoutMs) {
            failStream("buffering timeout");
        }
    } else if (s_state == PlayerState::Playing) {
        // Недовантаження рахуємо лише після стабільного відтворення, щоб
        // початкове наповнення буфера не давало хибних спрацювань.
        if (s_stableDone && pct < player_cfg::kBufferLowPercent) {
            s_underruns = s_underruns + 1;
            s_rebuffering = true;
            setState(PlayerState::Buffering, "buffer underrun");
        }
    }
}

void applyMutePolicy(uint32_t now) {
    switch (s_state) {
        case PlayerState::Playing:
            if (!s_stableDone && now - s_playingSinceMs >= player_cfg::kUnmuteStableMs) {
                s_stableDone = true;
                s_backoffMs = player_cfg::kReconnectInitialMs;  // потік стабільний
                muteDac(false);
            }
            break;
        case PlayerState::Buffering:
            break;  // мʼют не чіпаємо: старт лишається замʼютованим, просідання — ні
        default:  // Idle, Connecting, Error, Reconnecting
            if (!s_dacMuted) {
                muteDac(true);
            }
            s_stableDone = false;
            break;
    }
}

void updateState(uint32_t now) {
    switch (s_state) {
        case PlayerState::Error:
            if (s_wantPlaying && timeReached(now, s_nextRetryMs)) {
                s_reconnectCount = s_reconnectCount + 1;
                connectNow(true);
            }
            break;
        case PlayerState::Buffering:
        case PlayerState::Playing:
            monitorStream(now);
            break;
        default:
            break;
    }
    applyMutePolicy(now);
}

void taskEntry(void*) {
    AudioPlayer::taskLoop();
    vTaskDelete(nullptr);
}

#if AUDIO_PLAYER_CB_STYLE == 2
void onAudioMsg(Audio::msg_t m) {
    switch (m.e) {
        case Audio::evt_info:        handleInfo(m.msg); break;
        case Audio::evt_bitrate:     handleBitrate(m.msg); break;
        case Audio::evt_eof:         handleEof(m.msg); break;
        case Audio::evt_name:        handleStation(m.msg); break;
        case Audio::evt_streamtitle: handleTitle(m.msg); break;
        case Audio::evt_id3data:     handleId3(m.msg); break;
        default:                     break;
    }
}
#endif

}  // namespace

// ===========================================================================
// Callback-и бібліотеки, стиль 1 (глобальні функції зі спеціальними іменами)
// ===========================================================================
#if AUDIO_PLAYER_CB_STYLE == 1
void audio_info(const char* info) { handleInfo(info); }
void audio_showstation(const char* info) { handleStation(info); }
void audio_showstreamtitle(const char* info) { handleTitle(info); }
void audio_bitrate(const char* info) { handleBitrate(info); }
void audio_eof_stream(const char* info) { handleEof(info); }
void audio_id3data(const char* info) { handleId3(info); }
#endif

// [Prompt 17] Хук перехоплення PCM для VU (audio_process_i2s) винесено в окремий файл
// audio/vu_pcm_hook.cpp: тут Audio.h оголошує функцію як weak, і визначення в цьому
// файлі теж стало б слабким (лінкер брав заглушку бібліотеки).


// ===========================================================================
// AudioPlayer
// ===========================================================================

bool AudioPlayer::begin(AudioProcessor* processorOrNull) {
    if (s_task != nullptr) {
        return true;
    }

    s_proc = processorOrNull;

    // XSMT: ззовні pulldown, тож при живленні ЦАП замʼютований; явно тримаємо LOW.
    pinMode(pins::kXsmt, OUTPUT);
    digitalWrite(pins::kXsmt, player_cfg::kXsmtMutedLevel);
    s_dacMuted = true;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        Serial.println("[PLAYER] mutex create failed");
        return false;
    }
    s_station[0] = '\0';
    s_title[0] = '\0';
    s_pendingUrl[0] = '\0';
    s_url[0] = '\0';

    // I2S ініціалізує бібліотека (конструктор + setPinout). MCLK/DIN не чіпаємо.
    s_audio = new (std::nothrow) Audio();
    if (s_audio == nullptr) {
        Serial.println("[PLAYER] Audio object alloc failed");
        return false;
    }
    s_audio->setPinout(pins::kI2sBclk, pins::kI2sWs, pins::kI2sDout);
#if AUDIO_PLAYER_HAS_SETBUFSIZE
    s_audio->setBufsize(player_cfg::kStreamBufRamBytes, player_cfg::kStreamBufPsramBytes);
#endif
    s_audio->setConnectionTimeout(player_cfg::kHttpTimeoutMs, player_cfg::kHttpsTimeoutMs);
    // Гучність бібліотеки — максимум і надалі не змінюється (керує AudioProcessor).
    s_audio->setVolume(player_cfg::kLibVolume);

#if AUDIO_PLAYER_CB_STYLE == 2
    Audio::audio_info_callback = onAudioMsg;
#endif

    s_state = PlayerState::Idle;
    s_stateSinceMs = millis();

    const BaseType_t ok = xTaskCreatePinnedToCore(
        taskEntry, "audio_player", player_cfg::kTaskStackBytes, nullptr,
        player_cfg::kTaskPriority, &s_task, player_cfg::kTaskCore);
    if (ok != pdPASS) {
        s_task = nullptr;
        Serial.println("[PLAYER] task create failed");
        return false;
    }

    Serial.printf("[PLAYER] ready (core %d, prio %u, processor %s, buffer %s)\n",
                  player_cfg::kTaskCore, static_cast<unsigned>(player_cfg::kTaskPriority),
                  (s_proc != nullptr) ? "attached" : "none",
                  AUDIO_PLAYER_HAS_SETBUFSIZE ? "configured" : "library default");
    return true;
}

bool AudioPlayer::playUrl(const char* url) {
    if (s_task == nullptr || url == nullptr || url[0] == '\0') {
        return false;
    }
    const size_t n = strlen(url);
    if (n >= player_cfg::kUrlMax) {
        return false;
    }
    Guard g;
    if (!g.ok()) {
        return false;
    }
    memcpy(s_pendingUrl, url, n + 1);
    s_cmd = Cmd::Play;
    return true;
}

bool AudioPlayer::stop() {
    if (s_task == nullptr) {
        return false;
    }
    Guard g;
    if (!g.ok()) {
        return false;
    }
    s_cmd = Cmd::Stop;
    return true;
}

bool AudioPlayer::isPlaying() {
    const PlayerState st = s_state;
    return st == PlayerState::Playing || st == PlayerState::Buffering;
}

// [Prompt 25] Рівень виходу декодера: чиста робота зі станом output_trim (без мʼютекса плеєра,
// без звернень до бібліотеки Audio), тож безпечно з будь-якої задачі.
void AudioPlayer::setOutputTrimDb(int8_t db) {
    output_trim::setTargetDb(db);
}

int8_t AudioPlayer::outputTrimDb() {
    return output_trim::targetDb();
}

void AudioPlayer::taskLoop() {
    for (;;) {
        processCommands();
#if AUDIO_PLAYER_CALL_LOOP
        // Бібліотека сама блокує там, де потрібно (внутрішній буфер).
        s_audio->loop();
#endif
#if EQ_SAMPLE_RATE_FROM_LIB
        // [Prompt 30] Частота дискретизації потоку для еквалайзера (хук audio_process_i2s її не
        // отримує). Викликається з тієї ж задачі, що й loop(), тож гонки з бібліотекою немає.
        // eq::setSampleRate() ігнорує значення поза межами (0 до розбору заголовка).
        eq::setSampleRate(s_audio->getSampleRate());
#endif
        updateState(millis());
        vTaskDelay(msToTicksAtLeastOne(player_cfg::kLoopDelayMs));
    }
}

PlayerState AudioPlayer::state() { return s_state; }

const char* AudioPlayer::stateName(PlayerState s) {
    switch (s) {
        case PlayerState::Idle:         return "Idle";
        case PlayerState::Connecting:   return "Connecting";
        case PlayerState::Playing:      return "Playing";
        case PlayerState::Buffering:    return "Buffering";
        case PlayerState::Error:        return "Error";
        case PlayerState::Reconnecting: return "Reconnecting";
    }
    return "?";
}

bool AudioPlayer::currentMetadata(char* stationOut, size_t stationCap, char* titleOut,
                                  size_t titleCap) {
    if (stationOut != nullptr && stationCap > 0) stationOut[0] = '\0';
    if (titleOut != nullptr && titleCap > 0) titleOut[0] = '\0';

    Guard g;
    if (!g.ok()) {
        return false;
    }
    copyUtf8Trunc(stationOut, stationCap, s_station);
    copyUtf8Trunc(titleOut, titleCap, s_title);
    return s_station[0] != '\0' || s_title[0] != '\0';
}

uint32_t AudioPlayer::reconnectCount() { return s_reconnectCount; }

uint32_t AudioPlayer::bufferUnderrunCount() { return s_underruns; }

int AudioPlayer::lastHttpCode() { return s_httpCode; }

const char* AudioPlayer::normalizeIcy(const char* in, char* out, size_t cap) {
    return icyToUtf8(in, out, cap);
}