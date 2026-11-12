// Аудіоплеєр інтернет-радіо (ESP32-audioI2S).
//
// ЗАЛЕЖНІСТЬ ВІД ЗОВНІШНЬОЇ БІБЛІОТЕКИ: у файлі використано такі елементи API
// ESP32-audioI2S (їх треба звірити з Audio.h вашої версії при першій збірці):
//   Audio(), setPinout(), setBufsize() [лише при AUDIO_PLAYER_HAS_SETBUFSIZE],
//   setConnectionTimeout(), setVolume(), connecttohost(), stopSong(), loop(),
//   isRunning(), inBufferFilled(), inBufferFree(), setOutputSampleRate() [P44], getSampleRate() [P30, лише при
//   EQ_SAMPLE_RATE_FROM_LIB], а також callback-и (стиль обирає AUDIO_PLAYER_CB_STYLE):
//   стиль 1: audio_info, audio_showstation, audio_showstreamtitle, audio_bitrate,
//            audio_eof_stream, audio_id3data;
//   стиль 2: Audio::audio_info_callback, Audio::msg_t, Audio::evt_info,
//            evt_bitrate, evt_eof, evt_name, evt_streamtitle, evt_id3data;

// [DEF6] ДОДАНО: монітор «хвоста» файлу (tailMonitor, лише USB_HOST_DIAG), сторожовий таймер кінця файлу (stalledNearEnd /
// synthesizeFileEof, FILE_EOF_WATCHDOG) у monitorFile, лічильники для usb.eof (AudioPlayer::fileEofStats). Справжній шлях eof
// (handleEof -> s_evtFileEof -> kEndTailMs -> finishFile) не змінено; радіо не чіпається.
// [DEF5b] ДОДАНО в connectNow() виклик ssl_preflight::beforeConnect() ПЕРЕД HEAPMAP_POINT(C6) / connecttohost() (запобіжна
// перевірка буферів TLS після USB-сесії; RestartRequested -> failStream + довга пауза замість підключення).
// [DEF5a] ДОДАНО лише HEAPMAP_POINT(C6) і HEAPMAP_CONNECT_RESULT(ok) у connectNow() (порожні при USB_HEAPMAP = 0).
// [USB13b] ДОДАНО виклики ssl_workaround:: (noteFilePlayed / onRadioConnect / noteLibSslAllocText): обхідне рішення DEF-5.
// [USB7] ДОДАНО файлове джерело (AudioPlayer::playFile, Audio::connecttoFS(usb_fs::volume(), ...)): окремі гілки
// doPlayFile / failFile / finishFile / monitorFile; радіо-логіка (reconnect, backoff, буфер, недовантаження) для
// файлу НЕ виконується (s_source == Source::File). Решта файлу для радіо — без змін.

// [USB18a] ДОДАНО: ID3-теги файлу (handleId3 -> s_tags під критичним розділом s_tagMux; скидання в doPlayFile; fileTags()),
// seek за кінець файлу (seekFileSec(sec, atEnd) + seekToEnd() у processSeek: синтез кінця треку без read_error), лічильники
// (USB_HOST_DIAG). Друк із handleId3 прибрано (колбек бібліотеки не друкує): теги логуються з задачі плеєра (tagLogPoll).

#include "audio/audio_player.h"

#include <Audio.h>

#include <driver/gpio.h>  // [X1b] gpio_set_level
#include <atomic>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "audio/eq.h"
#include "audio/line_path.h"  // [USB7] передумова playFile: прохід зовнішніх входів не піднято
#include "audio/output_trim.h"
#include "audio/radio_volume.h"  // [USB7] цифровий мʼют радіо: AppController тримає його після паузи (s_stopHold)
#include "audio/xsmt_trace.h"            // [Prompt X1]
#include "config/audio_player_config.h"
#include "config/eq_config.h"
#include "config/features.h"            // [USB7] FEATURE_USB_HOST
#include "config/file_player_config.h"  // [USB7]
#include "config/line_path_config.h"  // [Prompt 48d] LINE_PATH_ENABLE (раніше ADC_VU_ENABLE через adc_vu_config.h)
#include "config/pins.h"
#include "config/ssl_preflight_config.h"  // [DEF5b] kRetryHoldMs
#include "config/xsmt_config.h"          // [Prompt X1]
#include "config/player_mode_config.h"   // [USB14] kPauseFadeMs
#include "core/ssl_preflight.h"          // [DEF5b] запобіжна перевірка буферів TLS перед https-підключенням
#include "core/ssl_workaround.h"         // [USB13b] WORKAROUND DEF-5: облік невдач підключення радіо після файлу
#include "usb/usb_heapmap.h"            // [DEF5a] HEAPMAP_POINT / HEAPMAP_CONNECT_RESULT (порожні при USB_HEAPMAP = 0)
#if FEATURE_USB_HOST
#include "usb/usb_fs.h"         // [USB7] usb_fs::volume() для connecttoFS
#include "usb/usb_msc_mount.h"  // [USB7] isMounted()
#if FEATURE_USB_HOST && USB_HOST_DIAG
#include "usb/usb_memdiag.h"  // [USB13] знімки пам'яті навколо підключення радіо (DEF-5)
#endif
#endif

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

enum class Cmd : uint8_t { None, Play, Stop, PlayFile };  // [USB7] + PlayFile

// [Prompt 44] Запит на зміну фіксації частоти I2S0: 0 — немає, 1 — 48 кГц, 2 — частота джерела.
// Пише setFixedOutputRate() (будь-яка задача), виконує й скидає задача плеєра.
volatile uint8_t s_clkReq = 0;
volatile bool s_clkFixed = false;

Audio* s_audio = nullptr;
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
volatile bool s_evtSampleRate = false;   // [X1d] бібліотека надрукувала "SampleRate (Hz): N" (декодер встановив частоту)
volatile uint32_t s_rateEvtAtMs = 0;

// [USB7] Файлове джерело. Джерело (Radio / File) і прапорці пише лише задача плеєра (крім s_evtFileEof — callback
// бібліотеки в контексті Audio::loop()).
enum class Source : uint8_t { Radio, File };
volatile Source s_source = Source::Radio;
// true між успішним connecttoFS і зупинкою / збоєм / кінцем: лише тоді eof-подія вважається ПРИРОДНИМ кінцем файлу.
// Скидається ДО кожного власного stopSong(), тож eof від нашої зупинки не плутається з кінцем файлу.
volatile bool s_fileActive = false;
volatile bool s_evtFileEof = false;       // callback: природний eof файлу
std::atomic<bool> s_fileEnded{false};     // прапорець «файл закінчився» (takeFileEnded)
volatile uint32_t s_fileEndedAtMs = 0;
volatile uint32_t s_filePosSec = 0;       // кеш позиції / тривалості (оновлює задача плеєра)
volatile uint32_t s_fileDurSec = 0;
volatile bool s_seekPending = false;
volatile int32_t s_seekSec = 0;
volatile bool s_seekEndPending = false;  // [USB18a] seek за кінець файлу: виконує задача плеєра (seekToEnd)
// [USB14] Пауза файлу: запит (0 — немає, 1 — пауза, 2 — продовження; пише API, виконує й скидає задача плеєра) і стан
// (пише лише задача плеєра; читається з будь-якої).
volatile uint8_t s_pauseReq = 0;
volatile bool s_filePaused = false;
const char* volatile s_fileError = nullptr;  // статичний рядок
// Під мʼютексом: запит playFile і шлях активного / останнього файлу.
char s_pendingFile[file_player_cfg::kPathMax];
#if FEATURE_USB_HOST && USB_HOST_DIAG
volatile bool s_dbgFileSeen = false;  // [USB13] true після першого успішного connecttoFS (знімки пам'яті навколо радіо)
#endif
int32_t s_pendingStartSec = -1;
char s_filePath[file_player_cfg::kPathMax];
// Лише задача плеєра. [USB7] Цифровий мʼют radio_volume: після паузи радіо AppController тримає його (s_stopHold до
// наступного startStream()), тож на час файлу плеєр сам його знімає і ПОВЕРТАЄ при зупинці / кінці / збої.
volatile uint32_t s_fxExtraSettleMs = file_player_cfg::kExtraSettleMs;      // usb.fx extra
volatile int32_t s_fxDigDelayMs = static_cast<int32_t>(file_player_cfg::kDigitalUnmuteDelayMs);  // usb.fx dig
volatile int32_t s_fxLevelDb = file_player_cfg::kFileLevelDb;                // usb.fx level
bool s_fileRadioMuteTouched = false;
bool s_fileRadioPrevMuted = true;
bool s_fileDigitalUnmuted = false;  // radio_volume вже розмʼючено для поточного файлу
bool s_fileDacOpenSeen = false;     // XSMT уже високо для поточного файлу
uint32_t s_fileDacOpenAtMs = 0;
bool s_fileEofPending = false;
uint32_t s_fileEofAtMs = 0;
uint32_t s_filePosPollMs = 0;
// [DEF6] Сторожовий таймер кінця файлу / монітор «хвоста» (лише задача плеєра; скидаються в doPlayFile / processSeek / паузі).
bool s_wdPosValid = false;       // s_wdPosSec / s_wdPosChangedMs актуальні
uint32_t s_wdPosSec = 0;         // остання побачена позиція (кеш, секунди)
uint32_t s_wdPosChangedMs = 0;   // millis(), коли позиція востаннє змінилась (або почався відлік)
#if FEATURE_USB_HOST && USB_HOST_DIAG
uint32_t s_tailLastLogMs = 0;    // останній рядок «tail» (мс)
uint8_t s_tailLines = 0;         // скільки рядків «tail» надруковано для поточного треку
bool s_wdStallLogged = false;    // FILE_EOF_WATCHDOG = 0: завмирання цього треку вже зафіксовано
volatile uint32_t s_eofRealCount = 0;        // справжні eof, оброблені monitorFile
volatile uint32_t s_eofSynthCount = 0;       // синтезовані eof
volatile uint32_t s_stallNoActionCount = 0;  // завмирання при FILE_EOF_WATCHDOG = 0
volatile bool s_lastStallValid = false;
volatile bool s_lastStallSynth = false;
volatile uint32_t s_lastStallCur = 0;
volatile uint32_t s_lastStallDur = 0;
volatile uint32_t s_lastStallInbuf = 0;
volatile uint32_t s_lastStallMs = 0;
char s_lastStallName[file_player_cfg::kEofStatNameMax] = {0};
#endif

// Тільки задача плеєра.
char s_url[player_cfg::kUrlMax];
bool s_wantPlaying = false;
bool s_dacMuted = true;
bool s_stableDone = false;    // [X1] Playing тривав >= kUnmuteStableMs (лише backoff і детектор недовантаження; XSMT від нього НЕ залежить)
uint32_t s_rateSeen = 0;      // [X1] остання частота Audio::getSampleRate() (0 — невідома)
uint32_t s_rateSinceMs = 0;   // [X1] момент, коли частота набула поточного значення
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

void muteDac(bool mute, const char* why) {
    digitalWrite(pins::kXsmt,
                 mute ? player_cfg::kXsmtMutedLevel : player_cfg::kXsmtUnmutedLevel);
    if (mute != s_dacMuted) {
        PLAYER_LOG("[PLAYER] XSMT %s\n", mute ? "mute" : "unmute");
        XSMT_EVT("player XSMT %s (%s)", mute ? "LOW" : "HIGH", why);
    }
    s_dacMuted = mute;
}

// [X1] XSMT LOW, а якщо ЦАП був розмʼючений — ще й пауза kMuteLeadMs (soft-mute ЦАП має відпрацювати) ПЕРЕД
// будь-якою зупинкою / перемиканням потоку (stopSong, connecttohost). Блокує лише задачу плеєра (ядро 1).
void muteDacWithLead(const char* why) {
    const bool wasOpen = !s_dacMuted;
    muteDac(true, why);
    if (wasOpen) {
        const uint32_t ms = xsmt::muteLeadMs();
        if (ms > 0) {
            vTaskDelay(msToTicksAtLeastOne(ms));
        }
    }
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
    XSMT_EVT("player state %s -> %s", AudioPlayer::stateName(old), AudioPlayer::stateName(ns));
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
    if (strstr(text, "32512") != nullptr || strstr(text, "Memory allocation failed") != nullptr) {
        ssl_workaround::noteLibSslAllocText();  // [USB13b] лише лічильник / лог (рішення не залежить)
    }
    if (strstr(text, player_cfg::kStreamReadyText) != nullptr) {
        s_evtStreamReady = true;
    }
    if (strstr(text, "SampleRate (Hz)") != nullptr) {  // [X1d]
        s_rateEvtAtMs = millis();
        s_evtSampleRate = true;
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
    // [USB7] Файлове джерело: eof ПРИХОДИТЬ ПІСЛЯ stopSong() з іменем файлу (контракт, розділ 11). Радіо-прапорець
    // s_evtEof (-> failStream -> reconnect) для файлу НЕ чіпаємо. Природний кінець — лише поки s_fileActive.
    if (s_source == Source::File) {
        if (s_fileActive) {
            s_evtFileEof = true;
            PLAYER_LOG("[PLAYER] file eof: %s\n", (text != nullptr) ? text : "?");
        } else {
            PLAYER_LOG("[PLAYER] file eof ignored (own stop): %s\n", (text != nullptr) ? text : "?");
        }
        return;
    }
    s_evtEof = true;
    PLAYER_LOG("[PLAYER] eof: %s\n", (text != nullptr) ? text : "?");
}

// --- [USB18a] ID3-теги файлу ---------------------------------------------------------------------------------------------
// Колбек бібліотеки може виконуватись не в задачі плеєра (скидання в doPlayFile — у задачі плеєра, читання — з async_tcp), тож
// усі доступи до s_tags — під КОРОТКИМ критичним розділом (портовий спінлок, копіювання ≈ 200 Б, без друку й виділень).
// Розбір і перевірка UTF-8 виконуються ДО входу в розділ (на стеку).
portMUX_TYPE s_tagMux = portMUX_INITIALIZER_UNLOCKED;
struct TagStore {
    uint32_t pathHash;  // хеш шляху файлу, якому належать теги
    char title[file_player_cfg::kTagMaxBytes];
    char artist[file_player_cfg::kTagMaxBytes];
    char album[file_player_cfg::kTagMaxBytes];
};
TagStore s_tags = {};
volatile uint32_t s_tagVer = 0;  // змінюється при кожному записі / скиданні (лише для логу з задачі плеєра)
uint32_t s_tagLoggedVer = 0;
uint32_t s_tagSeenVer = 0;
uint32_t s_tagSeenMs = 0;
#if FEATURE_USB_HOST && USB_HOST_DIAG
volatile uint32_t s_tagRejected = 0;   // теги з недійсним UTF-8 (відкинуто)
volatile uint32_t s_seekEndCount = 0;  // seek-и за кінець файлу (atEnd)
#endif

uint32_t pathHash(const char* p) {  // FNV-1a
    uint32_t h = 2166136261u;
    for (; p != nullptr && *p != '\0'; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619u;
    }
    return h;
}

// Строга перевірка UTF-8 (без надкоротких форм, сурогатів і значень > U+10FFFF).
bool validUtf8(const char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (size_t k = 1; k < len; ++k) {
            const uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return false;
        }
        i += len;
    }
    return true;
}

// Значення тегу -> out (обрізано по межі символу). false: порожнє або недійсний UTF-8 (останнє рахується).
bool extractTag(const char* v, char* out) {
    size_t n = strlen(v);
    while (n > 0 && static_cast<uint8_t>(v[n - 1]) <= 0x20) {
        --n;
    }
    while (n > 0 && static_cast<uint8_t>(*v) <= 0x20) {
        ++v;
        --n;
    }
    if (n == 0) {
        return false;
    }
    if (!validUtf8(v, n)) {
#if FEATURE_USB_HOST && USB_HOST_DIAG
        s_tagRejected = s_tagRejected + 1;
#endif
        return false;
    }
    if (n >= file_player_cfg::kTagMaxBytes) {
        n = file_player_cfg::kTagMaxBytes - 1;
        while (n > 0 && (static_cast<uint8_t>(v[n]) & 0xC0) == 0x80) {
            --n;  // v[n] — продовження: відступаємо до початку символу
        }
    }
    memcpy(out, v, n);
    out[n] = '\0';
    return n > 0;
}

// Скидання на старті файлу: теги попереднього не «протікають»; хеш шляху привʼязує нові теги до цього файлу.
void tagsReset(const char* path) {
    const uint32_t h = pathHash(path);
    portENTER_CRITICAL(&s_tagMux);
    s_tags.pathHash = h;
    s_tags.title[0] = '\0';
    s_tags.artist[0] = '\0';
    s_tags.album[0] = '\0';
    s_tagVer = s_tagVer + 1;
    portEXIT_CRITICAL(&s_tagMux);
}

// ГІПОТЕЗА: бібліотека віддає кожен тег окремим викликом «Title: …» / «Artist: …» / «Album: …» (формат звіряється за логом
// «[PLAYER] tags:» у hwcheck). Рядок, що не починається з відомого префікса, ігнорується. Перше непорожнє значення поля лишається.
void handleId3(const char* text) {
    if (text == nullptr || s_source != Source::File) {
        return;
    }
    int field = -1;
    size_t off = 0;
    if (strncmp(text, "Title: ", 7) == 0) {
        field = 0;
        off = 7;
    } else if (strncmp(text, "Artist: ", 8) == 0) {
        field = 1;
        off = 8;
    } else if (strncmp(text, "Album: ", 7) == 0) {
        field = 2;
        off = 7;
    } else {
        return;
    }
    char tmp[file_player_cfg::kTagMaxBytes];
    if (!extractTag(text + off, tmp)) {
        return;
    }
    portENTER_CRITICAL(&s_tagMux);
    char* dst = (field == 0) ? s_tags.title : (field == 1) ? s_tags.artist : s_tags.album;
    if (dst[0] == '\0') {
        memcpy(dst, tmp, strlen(tmp) + 1);
        s_tagVer = s_tagVer + 1;
    }
    portEXIT_CRITICAL(&s_tagMux);
}

// Лог тегів із ЗАДАЧІ плеєра (не з колбека): один рядок після того, як набір тегів не змінювався >= 500 мс.
void tagLogPoll(uint32_t now) {
    const uint32_t ver = s_tagVer;
    if (s_source != Source::File || ver == s_tagLoggedVer) {
        return;
    }
    if (ver != s_tagSeenVer) {
        s_tagSeenVer = ver;
        s_tagSeenMs = now;
        return;
    }
    if (static_cast<uint32_t>(now - s_tagSeenMs) < 500) {
        return;
    }
    TagStore snap;
    portENTER_CRITICAL(&s_tagMux);
    snap = s_tags;
    portEXIT_CRITICAL(&s_tagMux);
    s_tagLoggedVer = ver;
    Serial.printf("[PLAYER] tags: title='%s' artist='%s' album='%s'\n", snap.title, snap.artist, snap.album);
}

// --- Підключення й збої ---

// Планує наступну спробу з exponential backoff, мʼютить ЦАП, зупиняє потік.
void failStream(const char* reason) {
#if FEATURE_USB_HOST && USB_HOST_DIAG
    if (s_dbgFileSeen) {  // [USB13] стан пам'яті в момент збою радіо ПІСЛЯ файла (разом з хуком невдалих виділень)
        Serial.printf("[PLAYER] DIAG failStream reason='%s'\n", reason != nullptr ? reason : "?");
        usb_memdiag::print("player:failure", true);
        usb_memdiag::printFailStats();
    }
#endif
    muteDacWithLead("failure");  // [X1] спочатку мʼют (+пауза), потім stopSong()
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
    muteDac(true, "connect");
    s_rateSeen = 0;  // [X1] частота нового потоку ще невідома
    s_rateSinceMs = millis();
    s_stableDone = false;
    s_rebuffering = false;
    s_notRunning = false;
    s_evtEof = false;
    s_evtBitrate = false;
    s_evtStreamReady = false;
    s_evtSampleRate = false;  // [X1d]
    s_bufPercent = 0;

#if FEATURE_USB_HOST && USB_HOST_DIAG
    if (s_dbgFileSeen) {  // [USB13]
        usb_memdiag::print("player:connect begin (file played before)");
    }
#endif
    // [DEF5b] Перевірка ДО C6 (пробні виділення звільняються до знімка). Може заблокувати цю задачу до kMaxWaitMs (як і сам
    // connecttohost() блокує на секунди). RestartRequested: тихий перезапуск замовлено, підключатись НЕ можна — failStream()
    // (мʼют, stopSong, Error); довга пауза, щоб backoff 1 / 2 / 4 с не викликав повторних спроб до перезапуску. Stop від
    // beginRestart() обробить цикл задачі (processCommands) одразу після повернення звідси. ssl_workaround тут не бачить
    // невдачі (підключення не робилось) — лічильник його https-невдач не чіпається.
    if (ssl_preflight::beforeConnect(s_url) == ssl_preflight::Verdict::RestartRequested) {
        failStream("ssl preflight: no fit for TLS buffers, quiet restart");
        s_nextRetryMs = millis() + ssl_preflight_cfg::kRetryHoldMs;
        return;
    }
    HEAPMAP_POINT(C6);  // [DEF5a] безпосередньо перед connecttohost (знімається лише в вікні після виходу з USB)
    XSMT_EVT("player connecttohost begin");
    const bool ok = s_audio->connecttohost(s_url);
    s_connectedAtMs = millis();
    HEAPMAP_CONNECT_RESULT(ok);  // [DEF5a] C7: результат підключення (мс від кінця C6); при збої — блок [HM] FAIL
    XSMT_EVT("player connecttohost end ok=%d", ok ? 1 : 0);
    ssl_workaround::onRadioConnect(ok, s_url);  // [USB13b] WORKAROUND DEF-5 (рішення — у модулі; інакше нічого не змінює)
    if (!ok) {
        failStream("connecttohost failed");
        return;
    }
    setState(PlayerState::Buffering, "connected");
}

void fileRestoreRadioVolume();  // [USB7] визначено нижче (блок «Файлове джерело»)

void doPlay(const char* url) {
    XSMT_EVT("player doPlay (command received by player task)");
    // [USB7] Якщо грав файл: eof від нашого stopSong() не має вважатись його кінцем (s_fileActive = false ДО stopSong).
    s_fileActive = false;
    s_fileEofPending = false;
    s_evtFileEof = false;
    s_seekPending = false;
    s_filePaused = false;  // [USB14]
    s_pauseReq = 0;
    muteDacWithLead("play");  // [X1] ПОРЯДОК ВИПРАВЛЕНО: мʼют (+пауза) ДО stopSong(), раніше — після
    s_audio->stopSong();  // безпечно, якщо нічого не грало
    s_source = Source::Radio;  // [USB7] eof від stopSong() вище ще оброблено як файловий (ігнорується)
    s_fileRadioMuteTouched = false;  // [USB7] радіо стартує: цифровим мʼютом далі керує AppController (не повертаємо!)
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
    XSMT_EVT("player doStop (command received by player task)");
    // [USB7] Зупинка файлу: eof від stopSong() не є природним кінцем (прапорець «файл закінчився» НЕ піднімається).
    s_fileActive = false;
    s_fileEofPending = false;
    s_evtFileEof = false;
    s_seekPending = false;
    s_filePaused = false;  // [USB14] stopSong() знімає прапорець «running» бібліотеки
    s_pauseReq = 0;
    muteDacWithLead("stop");  // [X1] ПОРЯДОК ВИПРАВЛЕНО: мʼют (+пауза) ДО stopSong(), раніше — після
    s_audio->stopSong();
    fileRestoreRadioVolume();  // [USB7] лише якщо грав файл (інакше нічого не робить)
    s_stableDone = false;
    s_rebuffering = false;
    s_notRunning = false;
    clearMetadata();
    PLAYER_LOG("[PLAYER] stop\n");
    setState(PlayerState::Idle, "stop");
}

// ===========================================================================
// [USB7] Файлове джерело (виконується лише в задачі плеєра)
// ===========================================================================

bool fileElapsed(uint32_t now, uint32_t since, uint32_t ms) {
    return static_cast<int32_t>(now - since - ms) >= 0;
}

// Чому playFile зараз неможливий (nullptr — можна). Викликається і з API, і повторно із задачі плеєра.
// Радіо «активне», якщо його запитано (s_wantPlaying), line_path — якщо ресурси підняті, I2S0 зафіксовано на 48 кГц.
const char* fileBlockReason() {
    if (LinePath::resourcesUp() || LinePath::isRunning()) {
        return "line_path active";
    }
    if (s_clkFixed) {
        return "I2S0 fixed to 48 kHz (line_path owns it)";
    }
    if (s_wantPlaying) {
        return "radio active";
    }
#if FEATURE_USB_HOST
    if (!usb_msc_mount::isMounted()) {
        return "usb volume not mounted";
    }
    return nullptr;
#else
    return "usb host disabled (FEATURE_USB_HOST = 0)";
#endif
}

// Тримає цифровий мʼют радіо (radio_volume) ВКЛЮЧЕНИМ від старту файлу: попередній стан запамʼятовується один раз,
// миттєва тиша виконується першим блоком хука. Розмʼют — лише після розмʼюту ЦАП (fileUnmuteRadioVolume):
// [USB7, клацання на старті музики] ЦАП (XSMT) відкривається під цифровою тишею, а звук нарощує DSP (плавно),
// а не стрибком у середині сигналу.
void fileHoldRadioVolume() {
    if (!s_fileRadioMuteTouched) {
        s_fileRadioPrevMuted = radio_volume::muted();
        s_fileRadioMuteTouched = true;
    }
    radio_volume::setMute(true);
    radio_volume::requestForceMute();
    s_fileDigitalUnmuted = false;
    s_fileDacOpenSeen = false;
}

// Знімає цифровий мʼют радіо на час файлу: DSP плавно підніме звук (dsp_volume_cfg). Мʼют користувача / standby
// AppController сюди не передає: кнопка мʼюту повторно мʼютить радіо-DSP, тож заглушить і файл.
void fileUnmuteRadioVolume() {
    radio_volume::setMute(false);
    s_fileDigitalUnmuted = true;
}

// Повертає цифровий мʼют радіо як було ДО файлу. Викликати ПІСЛЯ мʼюту ЦАП (XSMT) і stopSong(): звуку вже немає,
// тож додатково просимо миттєвий мʼют для першого блоку наступного потоку (як це робить AppController).
void fileRestoreRadioVolume() {
    if (!s_fileRadioMuteTouched) {
        return;
    }
    s_fileRadioMuteTouched = false;
    s_fileDigitalUnmuted = false;
    radio_volume::setMute(s_fileRadioPrevMuted);  // як було ДО файлу (hold міг лишити мʼют увімкненим)
    if (s_fileRadioPrevMuted) {
        radio_volume::requestForceMute();
    }
}

// Збій файлу (читання, вийняття флешки, таймаут старту): мʼют, зупинка, стан Error. БЕЗ reconnect і backoff
// (s_wantPlaying для файлу завжди false, тож гілка Error -> connectNow радіо не спрацьовує).
void failFile(const char* reason) {
    s_fileActive = false;  // ДО stopSong(): eof від нашої зупинки не є кінцем файлу
    s_fileEofPending = false;
    s_evtFileEof = false;
    s_seekPending = false;
    s_filePaused = false;  // [USB14]
    s_pauseReq = 0;
    muteDacWithLead("file failure");
    s_audio->stopSong();
    fileRestoreRadioVolume();
    s_stableDone = false;
    s_rebuffering = false;
    s_notRunning = false;
    s_fileError = reason;
    Serial.printf("[PLAYER] file failure: %s (t=%lu ms, pos=%lu/%lu s)\n", reason,
                  static_cast<unsigned long>(millis()), static_cast<unsigned long>(s_filePosSec),
                  static_cast<unsigned long>(s_fileDurSec));
    setState(PlayerState::Error, reason);
}

// Природний кінець файлу (пауза kEndTailMs після eof вже минула): мʼют ЦАП, Idle, прапорець «файл закінчився».
void finishFile() {
    s_fileActive = false;
    s_evtFileEof = false;  // [DEF6] пізня подія бібліотеки після синтезованого eof не лишається «висіти»
    s_filePaused = false;  // [USB14] (на паузі eof не приходить; запобіжник)
    s_pauseReq = 0;
    muteDacWithLead("file end");
    s_audio->stopSong();  // бібліотека вже зупинилась; повторний виклик безпечний (як у doStop)
    fileRestoreRadioVolume();
    s_stableDone = false;
    s_notRunning = false;
    setState(PlayerState::Idle, "file ended");
    const uint32_t now = millis();
    s_fileEndedAtMs = now;
    s_fileEnded.store(true);
    Serial.printf("[PLAYER] file ended t=%lu ms (pos=%lu/%lu s)\n", static_cast<unsigned long>(now),
                  static_cast<unsigned long>(s_filePosSec), static_cast<unsigned long>(s_fileDurSec));
}

void doPlayFile(const char* path, int32_t startSec) {
    XSMT_EVT("player doPlayFile (command received by player task)");
    const char* why = fileBlockReason();  // стан міг змінитись між запитом і виконанням
    if (why != nullptr) {
        Serial.printf("[PLAYER] playFile refused in task: %s\n", why);
        return;
    }
#if FEATURE_USB_HOST
    // Поточне джерело (інший файл) зупиняється штатним шляхом: мʼют ЦАП (+пауза), eof від stopSong() ігнорується.
    s_fileActive = false;
    s_fileEofPending = false;
    s_evtFileEof = false;
    s_seekPending = false;
    s_filePaused = false;  // [USB14] новий файл (також «наступний» на паузі) грає без паузи
    s_pauseReq = 0;
    muteDacWithLead("play file");
    s_audio->stopSong();  // безпечно, якщо нічого не грало

    s_source = Source::File;
    s_wantPlaying = false;  // радіо-перепідключення для файлу вимкнено
    s_url[0] = '\0';
    s_backoffMs = player_cfg::kReconnectInitialMs;
    s_httpCode = 0;
    s_fileError = nullptr;
    s_fileEnded.store(false);
    s_fileEndedAtMs = 0;
    s_filePosSec = 0;
    s_fileDurSec = 0;
    s_filePosPollMs = millis();
    s_wdPosValid = false;  // [DEF6] новий файл: відлік завмирання з нуля
#if FEATURE_USB_HOST && USB_HOST_DIAG
    s_tailLines = 0;
    s_tailLastLogMs = 0;
    s_wdStallLogged = false;
#endif
    clearMetadata();
    tagsReset(path);  // [USB18a] теги попереднього файлу не «протікають»
#if FEATURE_USB_HOST && USB_HOST_DIAG
    Serial.printf("[PLAYER] DIAG usb18a: tags_rejected=%lu seek_end=%lu\n", static_cast<unsigned long>(s_tagRejected),
                  static_cast<unsigned long>(s_seekEndCount));
#endif
    {
        Guard g;
        if (g.ok()) {
            strncpy(s_filePath, path, sizeof(s_filePath) - 1);
            s_filePath[sizeof(s_filePath) - 1] = '\0';
        }
    }

    fileHoldRadioVolume();  // цифрова тиша ДО першого блоку нового файлу (розмʼют — після відкриття ЦАП)

    // Той самий скид службових прапорців, що й connectNow() для радіо (без радіо-таймерів).
    s_rateSeen = 0;
    s_rateSinceMs = millis();
    s_stableDone = false;
    s_rebuffering = false;
    s_notRunning = false;
    s_evtEof = false;
    s_evtBitrate = false;
    s_evtStreamReady = false;
    s_evtSampleRate = false;
    s_bufPercent = 0;

    // ПРИПУЩЕННЯ: рівень файлу = типовий рівень станції (див. file_player_cfg::kFileLevelDb).
    const int levelDb = static_cast<int>(s_fxLevelDb);
    output_trim::setTargetDb(static_cast<int8_t>(levelDb));

    Serial.printf("[PLAYER] play file: %s (start %ld s, level %d dB, extra %lu ms, dig %ld ms)\n", path,
                  static_cast<long>(startSec), levelDb, static_cast<unsigned long>(s_fxExtraSettleMs),
                  static_cast<long>(s_fxDigDelayMs));
    setState(PlayerState::Connecting, path);
    XSMT_EVT("player connecttoFS begin");
    const bool ok = s_audio->connecttoFS(usb_fs::volume(), path, startSec);
    s_connectedAtMs = millis();
    XSMT_EVT("player connecttoFS end ok=%d", ok ? 1 : 0);
    if (!ok) {
        failFile("connecttoFS failed");
        return;
    }
    s_fileActive = true;
    ssl_workaround::noteFilePlayed();  // [USB13b] «файл грав у сеансі» (усі збірки)
#if FEATURE_USB_HOST && USB_HOST_DIAG
    s_dbgFileSeen = true;  // [USB13]
#endif
    if (s_fxDigDelayMs < 0) {  // usb.fx dig -1: радіо-DSP розмʼючується одразу (поведінка до виправлення)
        fileUnmuteRadioVolume();
    }
    setState(PlayerState::Buffering, "file opened");
#else
    (void)path;
    (void)startSec;
#endif
}

// [USB18a] Seek за кінець файлу = кінець треку. Той самий шлях, що синтез eof у DEF6 (але БЕЗ залежності від
// FILE_EOF_WATCHDOG): зупинка в бібліотеці, s_fileEofPending -> після kEndTailMs finishFile() (мʼют ЦАП, Idle, прапорець
// «файл закінчився») -> usb_queue бачить takeFileEnded() і переходить далі за правилами repeat / shuffle / end_of_queue.
// s_fileActive лишається true до finishFile, тож «decoder stopped» / read_error не виникає.
void seekToEnd() {
    const int32_t delta = s_seekSec;
    if (!s_fileActive || s_fileEofPending || s_filePaused || (s_state != PlayerState::Playing && s_state != PlayerState::Buffering)) {
        Serial.println("[PLAYER] seek ignored: no file is playing");
        return;
    }
    Serial.printf("[PLAYER] seek past end -> end of track (cur=%lu dur=%lu delta=%ld)\n", static_cast<unsigned long>(s_filePosSec),
                  static_cast<unsigned long>(s_fileDurSec), static_cast<long>(delta));
    s_seekPending = false;
    s_evtFileEof = false;  // пізня справжня подія не має дублювати кінець
    s_audio->stopSong();
    s_fileEofPending = true;
    s_fileEofAtMs = millis();
    s_wdPosValid = false;
#if FEATURE_USB_HOST && USB_HOST_DIAG
    s_seekEndCount = s_seekEndCount + 1;
#endif
}

void processSeek() {
    if (s_seekEndPending) {  // [USB18a]
        s_seekEndPending = false;
        seekToEnd();
        return;
    }
    if (!s_seekPending) {
        return;
    }
    s_seekPending = false;
    const int32_t sec = s_seekSec;
    if (!s_fileActive || (s_state != PlayerState::Playing && s_state != PlayerState::Buffering)) {
        Serial.println("[PLAYER] seek ignored: no file is playing");
        return;
    }
    // Audio::setTimeOffset(int): семантика (відносний / абсолютний зсув) у закріпленому коміті не звірена.
    s_audio->setTimeOffset(static_cast<int>(sec));
    s_wdPosValid = false;  // [DEF6] після seek позиція може стрибнути / затриматись: відлік завмирання з нуля
#if FEATURE_USB_HOST && USB_HOST_DIAG
    s_tailLines = 0;  // [DEF6] монітор «хвоста» — знову для нового положення
#endif
    Serial.printf("[PLAYER] seek requested: setTimeOffset(%ld) at pos %lu s\n", static_cast<long>(sec),
                  static_cast<unsigned long>(s_filePosSec));
}

// [USB14] Пауза / продовження файлу (лише задача плеєра, поряд з loop() бібліотеки).
// Пауза: 1) цифровий мʼют radio_volume (плавно ≈ 20 мс), чекаємо kPauseFadeMs; 2) XSMT LOW (+ kMuteLeadMs, якщо ЦАП був
// відкритий); 3) pauseResume() — декодер стає, вихідні буфери очищено. Позиція фіксується (кеш). Продовження: «ЦАП закритий»
// і цифрова тиша як на старті файлу (fileHoldRadioVolume), відлік стабільності клоку від ЗАРАЗ (інакше XSMT HIGH одразу:
// s_rateSinceMs / s_playingSinceMs давні), pauseResume(); далі той самий розмʼют, що й на старті (applyMutePolicy +
// updateFileState). Мʼют користувача повертає AppController::usbModeService() (як і після старту файлу).
void processPause() {
    const uint8_t req = s_pauseReq;
    if (req == 0) {
        return;
    }
    s_pauseReq = 0;
    if (s_source != Source::File || !s_fileActive || s_fileEofPending) {
        Serial.println("[PLAYER] pause request ignored: no file is playing");
        return;
    }
    if (req == 1) {
        if (s_filePaused || s_state != PlayerState::Playing) {
            Serial.printf("[PLAYER] pause request ignored (paused=%d state=%s)\n", s_filePaused ? 1 : 0,
                          AudioPlayer::stateName(s_state));
            return;
        }
        radio_volume::setMute(true);  // плавно: DSP гасить звук ≈ 20 мс [P47a]
        vTaskDelay(msToTicksAtLeastOne(player_mode_cfg::kPauseFadeMs));
        muteDacWithLead("file pause");  // XSMT LOW (+ пауза kMuteLeadMs, якщо ЦАП був відкритий)
        if (s_audio->isRunning()) {
            s_audio->pauseResume();
        }
        s_filePosSec = static_cast<uint32_t>(s_audio->getAudioCurrentTime());
        s_notRunning = false;
        s_fileDigitalUnmuted = false;
        s_fileDacOpenSeen = false;
        s_filePaused = true;
        Serial.printf("[PLAYER] file paused at %lu/%lu s\n", static_cast<unsigned long>(s_filePosSec),
                      static_cast<unsigned long>(s_fileDurSec));
    } else {
        if (!s_filePaused) {
            Serial.println("[PLAYER] resume request ignored: not paused");
            return;
        }
        fileHoldRadioVolume();  // цифрова тиша ДО першого блоку; скидає s_fileDigitalUnmuted / s_fileDacOpenSeen
        const uint32_t now = millis();
        s_rateSinceMs = now;     // відлік kClockSettleMs для XSMT HIGH — від продовження
        s_playingSinceMs = now;  // і запасний таймаут rate_wait
        s_notRunning = false;
        if (!s_audio->isRunning()) {
            s_audio->pauseResume();
        }
        s_filePaused = false;
        Serial.printf("[PLAYER] file resumed at %lu/%lu s\n", static_cast<unsigned long>(s_filePosSec),
                      static_cast<unsigned long>(s_fileDurSec));
    }
}

void pollFilePosition(uint32_t now) {
    if (s_source != Source::File || !s_fileActive || s_fileEofPending) {
        return;
    }
    if (!fileElapsed(now, s_filePosPollMs, file_player_cfg::kPosPollMs)) {
        return;
    }
    s_filePosPollMs = now;
    s_filePosSec = static_cast<uint32_t>(s_audio->getAudioCurrentTime());
    s_fileDurSec = static_cast<uint32_t>(s_audio->getAudioFileDuration());
}

void processCommands() {
    Cmd cmd = Cmd::None;
    char url[player_cfg::kUrlMax];
    url[0] = '\0';
    char file[file_player_cfg::kPathMax];  // [USB7]
    file[0] = '\0';
    int32_t fileStart = -1;
    {
        Guard g;
        if (!g.ok()) {
            return;
        }
        cmd = s_cmd;
        s_cmd = Cmd::None;
        if (cmd == Cmd::Play) {
            memcpy(url, s_pendingUrl, sizeof(url));
        } else if (cmd == Cmd::PlayFile) {
            memcpy(file, s_pendingFile, sizeof(file));
            fileStart = s_pendingStartSec;
        }
    }
    if (cmd == Cmd::Play) {
        doPlay(url);
    } else if (cmd == Cmd::Stop) {
        doStop();
    } else if (cmd == Cmd::PlayFile) {
        doPlayFile(file, fileStart);
    }
}

// [Prompt 44] Виконується в задачі плеєра (там само, де loop()), лише при Idle.
void processClockRequest() {
    const uint8_t req = s_clkReq;
    if (req == 0) {
        return;
    }
    s_clkReq = 0;
    if (s_state != PlayerState::Idle || s_wantPlaying) {
        PLAYER_LOG("[PLAYER] output rate request ignored: player is not idle\n");
        return;
    }
    const bool fixed = (req == 1);
    s_audio->setOutputSampleRate(fixed ? Audio::SR_48000 : Audio::SR_ORIGIN);
    s_clkFixed = fixed;
    PLAYER_LOG("[PLAYER] output rate: %s\n", fixed ? "fixed 48 kHz" : "source rate");
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

// [X1] Відстеження частоти потоку (Audio::getSampleRate): її зміна = перенастроювання клоку I2S0.
void trackSampleRate(uint32_t now) {
    const uint32_t rate = s_audio->getSampleRate();
    if (rate != s_rateSeen) {
        s_rateSeen = rate;
        s_rateSinceMs = now;
        XSMT_EVT("player sample rate -> %lu Hz (I2S0 clock event)", static_cast<unsigned long>(rate));
    }
}

bool elapsedAtLeast(uint32_t now, uint32_t since, uint32_t ms) {
    return static_cast<int32_t>(now - since - ms) >= 0;
}

#if FILE_EOF_WATCHDOG || (FEATURE_USB_HOST && USB_HOST_DIAG)
// [DEF6] Відлік «позиція файлу не змінюється» (лише задача плеєра; кеш s_filePosSec оновлює pollFilePosition).
// s_wdPosValid = false скидає відлік: новий файл, seek, пауза / продовження.
void trackPosProgress(uint32_t now) {
    const uint32_t pos = s_filePosSec;
    if (!s_wdPosValid || pos != s_wdPosSec) {
        s_wdPosValid = true;
        s_wdPosSec = pos;
        s_wdPosChangedMs = now;
    }
}

// [DEF6] Скільки мс позиція не змінюється (відлік не раніше за вхід у Playing).
uint32_t posStalledMs(uint32_t now) {
    uint32_t base = s_wdPosChangedMs;
    if (static_cast<int32_t>(s_playingSinceMs - base) > 0) {
        base = s_playingSinceMs;
    }
    return now - base;
}
#endif

#if FEATURE_USB_HOST && USB_HOST_DIAG
// [DEF6] Монітор «хвоста» (лише лог). Викликається із задачі плеєра (не з колбека бібліотеки, без блокувань).
void tailMonitor(uint32_t now, bool running) {
    const uint32_t dur = s_fileDurSec;
    const uint32_t pos = s_filePosSec;
    if (dur == 0 || pos + file_player_cfg::kTailWatchSec < dur) {
        return;  // невідома тривалість або ще далеко від кінця
    }
    if (s_tailLines >= file_player_cfg::kTailLogMaxLines) {
        return;
    }
    if (s_tailLines != 0 && !elapsedAtLeast(now, s_tailLastLogMs, file_player_cfg::kTailLogMs)) {
        return;
    }
    ++s_tailLines;
    s_tailLastLogMs = now;
    Serial.printf("[PLAYER] tail: t=%lu/%lu s inbuf=%lu running=%d state=%s since_progress=%lu\n",
                  static_cast<unsigned long>(pos), static_cast<unsigned long>(dur),
                  static_cast<unsigned long>(s_audio->inBufferFilled()), running ? 1 : 0,
                  AudioPlayer::stateName(s_state), static_cast<unsigned long>(posStalledMs(now)));
}

// [DEF6] Запис останнього випадку завмирання для `usb.eof`.
void recordLastStall(uint32_t cur, uint32_t dur, uint32_t inbuf, uint32_t stalled, bool synthesized) {
    s_lastStallCur = cur;
    s_lastStallDur = dur;
    s_lastStallInbuf = inbuf;
    s_lastStallMs = stalled;
    s_lastStallSynth = synthesized;
    copyUtf8Trunc(s_lastStallName, sizeof(s_lastStallName), s_filePath);
    s_lastStallValid = true;
}
#endif

#if FILE_EOF_WATCHDOG || (FEATURE_USB_HOST && USB_HOST_DIAG)
// [DEF6] Завмирання «наприкінці файлу»: Playing (не пауза / seek / старт), декодер «працює» (isRunning), тривалість відома,
// до кінця <= kEofNearEndSec, а позиція не змінюється >= kEofStallMs. Лише локальні файли (monitorFile викликається лише для них).
bool stalledNearEnd(uint32_t now, bool running, uint32_t* stalledOut) {
    if (s_state != PlayerState::Playing || s_filePaused || !running) {
        return false;
    }
    if (s_pauseReq != 0 || s_seekPending || s_fileEofPending) {
        return false;
    }
    const uint32_t dur = s_fileDurSec;
    const uint32_t pos = s_filePosSec;
    if (dur == 0 || pos + file_player_cfg::kEofNearEndSec < dur) {
        return false;  // тривалість невідома — не синтезуємо; або ще далеко від кінця
    }
    const uint32_t stalled = posStalledMs(now);
    if (stalled < file_player_cfg::kEofStallMs) {
        return false;
    }
    *stalledOut = stalled;
    return true;
}
#endif

#if FILE_EOF_WATCHDOG
// [DEF6] Синтез кінця файлу тим самим шляхом, що й справжній eof: зупинка в бібліотеці (як usb.stopf / stopSong у finishFile),
// s_fileEofPending -> після kEndTailMs finishFile() (мʼют ЦАП, Idle, прапорець «файл закінчився») -> usb_queue бачить
// takeFileEnded() і переходить на наступний трек, а не вважає це ручною зупинкою (s_fileActive лишається true до finishFile).
void synthesizeFileEof(uint32_t now, uint32_t stalled) {
    const uint32_t cur = s_filePosSec;
    const uint32_t dur = s_fileDurSec;
    const uint32_t inbuf = s_audio->inBufferFilled();
    Serial.printf("[PLAYER] eof synthesized (stall at t=%lu/%lu s, inbuf=%lu, stalled=%lu ms)\n",
                  static_cast<unsigned long>(cur), static_cast<unsigned long>(dur), static_cast<unsigned long>(inbuf),
                  static_cast<unsigned long>(stalled));
#if FEATURE_USB_HOST && USB_HOST_DIAG
    s_eofSynthCount = s_eofSynthCount + 1;
    recordLastStall(cur, dur, inbuf, stalled, true);
#endif
    s_evtFileEof = false;  // пізня справжня подія не має дублювати кінець
    s_audio->stopSong();
    s_fileEofPending = true;
    s_fileEofAtMs = now;
}
#elif FEATURE_USB_HOST && USB_HOST_DIAG
// [DEF6] FILE_EOF_WATCHDOG = 0: лише фіксуємо завмирання (раз на трек), нічого не робимо.
void noteStallNoAction(uint32_t stalled) {
    if (s_wdStallLogged) {
        return;
    }
    s_wdStallLogged = true;
    const uint32_t cur = s_filePosSec;
    const uint32_t dur = s_fileDurSec;
    const uint32_t inbuf = s_audio->inBufferFilled();
    s_stallNoActionCount = s_stallNoActionCount + 1;
    recordLastStall(cur, dur, inbuf, stalled, false);
    Serial.printf("[PLAYER] stall at end detected, FILE_EOF_WATCHDOG=0 -> no action (t=%lu/%lu s, inbuf=%lu, stalled=%lu ms)\n",
                  static_cast<unsigned long>(cur), static_cast<unsigned long>(dur), static_cast<unsigned long>(inbuf),
                  static_cast<unsigned long>(stalled));
}
#endif

// [USB7] Нагляд за файлом (Buffering / Playing). Замість monitorStream(): без reconnect, буфера й недовантажень.
void monitorFile(uint32_t now) {
    // Подія eof (прийшла ПІСЛЯ stopSong() бібліотеки). Якщо флешки вже немає — це НЕ кінець файлу (обгортка при
    // розмонтуванні віддає 0 байт, бібліотека бачить «eof»): збій, а не прапорець «файл закінчився».
    if (s_evtFileEof && !s_fileEofPending) {
        s_evtFileEof = false;
#if FEATURE_USB_HOST
        if (!usb_msc_mount::isMounted()) {
            failFile("volume removed (eof)");
            return;
        }
#endif
        s_fileEofPending = true;
        s_fileEofAtMs = now;
#if FEATURE_USB_HOST && USB_HOST_DIAG
        s_eofRealCount = s_eofRealCount + 1;  // [DEF6]
#endif
        const uint32_t pos = s_filePosSec;
        const uint32_t dur = s_fileDurSec;
        Serial.printf("[PLAYER] file eof event t=%lu ms (pos=%lu/%lu s)%s\n", static_cast<unsigned long>(now),
                      static_cast<unsigned long>(pos), static_cast<unsigned long>(dur),
                      (dur > 0 && pos + 3 < dur) ? " EARLY (read error?)" : "");
    }
    if (s_fileEofPending) {
        if (elapsedAtLeast(now, s_fileEofAtMs, file_player_cfg::kEndTailMs)) {
            s_fileEofPending = false;
            finishFile();
        }
        return;
    }
#if FEATURE_USB_HOST
    if (!usb_msc_mount::isMounted()) {
        failFile("volume removed");
        return;
    }
#endif

    if (s_filePaused) {  // [USB14] пауза: декодер свідомо стоїть, це не збій («decoder stopped» не рахується)
        s_notRunning = false;
        s_wdPosValid = false;  // [DEF6] після продовження відлік завмирання з нуля
        return;
    }
    const bool running = s_audio->isRunning();
    if (!running) {
        if (!s_notRunning) {
            s_notRunning = true;
            s_notRunningSinceMs = now;
        } else if (elapsedAtLeast(now, s_notRunningSinceMs, file_player_cfg::kNotRunningGraceMs)) {
            failFile("decoder stopped");
            return;
        }
    } else {
        s_notRunning = false;
    }

    // [DEF6] Відлік прогресу позиції; монітор «хвоста» (USB_HOST_DIAG); сторожовий таймер кінця файлу.
#if FILE_EOF_WATCHDOG || (FEATURE_USB_HOST && USB_HOST_DIAG)
    trackPosProgress(now);
#endif
#if FEATURE_USB_HOST && USB_HOST_DIAG
    tailMonitor(now, running);
#endif
#if FILE_EOF_WATCHDOG || (FEATURE_USB_HOST && USB_HOST_DIAG)
    {
        uint32_t stalled = 0;
        if (stalledNearEnd(now, running, &stalled)) {
#if FILE_EOF_WATCHDOG
            synthesizeFileEof(now, stalled);
            return;
#else
            noteStallNoAction(stalled);
#endif
        }
    }
#endif

    if (s_state == PlayerState::Buffering) {
        // Для файлу «готовий» = декодер щось повідомив (bitrate / SampleRate / stream ready) або минув запасний час.
        const bool ready = running && (s_evtBitrate || s_evtStreamReady || s_evtSampleRate ||
                                       elapsedAtLeast(now, s_connectedAtMs, file_player_cfg::kAssumePlayingMs));
        if (ready) {
            setState(PlayerState::Playing, "file ready");
        } else if (elapsedAtLeast(now, s_stateSinceMs, file_player_cfg::kStartTimeoutMs)) {
            failFile("start timeout");
        }
    }
}

void applyMutePolicy(uint32_t now) {
    switch (s_state) {
        case PlayerState::Playing:
            // [X1] XSMT HIGH, щойно потік Playing і клок I2S0 стабільний kClockSettleMs (частота не змінювалась).
            // Раніше — лише через kUnmuteStableMs = 3000 мс стабільного Playing (головна причина затримки першого звуку).
            if (s_dacMuted && !s_filePaused) {  // [USB14] на паузі ЦАП лишається замʼютованим
                // [X1b] За логом власника: "stream ready" (-> Playing) приходить РАНІШЕ, ніж декодер розбирає заголовок і
                // перемикає клок I2S0 (48000 -> 44100 через ≈ 37 мс після XSMT HIGH -> клацання на старті радіо). Тому
                // потрібна ще й подія bitrate (приходить після встановлення частоти); без неї — лише таймаут rate_wait.
                // [X1d] Замість очікування bitrate (≈ +250 мс після зміни частоти) чекаємо рядок "SampleRate (Hz)": його
                // бібліотека друкує одразу при встановленні частоти. Відлік kClockSettleMs — від пізнішої з двох подій
                // (зміна частоти за getSampleRate / цей рядок).
                const bool rateKnown = (s_rateSeen != 0) && s_evtSampleRate;
                const uint32_t refMs = (static_cast<int32_t>(s_rateEvtAtMs - s_rateSinceMs) > 0) ? s_rateEvtAtMs : s_rateSinceMs;
                // [USB7] Для файлу додається налаштована пауза (usb.fx extra); для радіо extra = 0 (поведінка не змінена).
                const uint32_t extra = (s_source == Source::File) ? s_fxExtraSettleMs : 0;
                const bool clockOk = rateKnown && elapsedAtLeast(now, refMs, xsmt::clockSettleMs() + extra);
                const bool rateTimeout = elapsedAtLeast(now, s_playingSinceMs, xsmt::rateWaitMaxMs() + extra);
                if (clockOk || rateTimeout) {
                    muteDac(false, clockOk ? "clock stable" : "sample rate unknown, timeout");
                }
            }
            // Стабільність потоку (скидання backoff, початок обліку недовантажень) — окремо від XSMT.
            if (!s_stableDone && now - s_playingSinceMs >= player_cfg::kUnmuteStableMs) {
                s_stableDone = true;
                s_backoffMs = player_cfg::kReconnectInitialMs;  // потік стабільний
            }
            break;
        case PlayerState::Buffering:
            break;  // мʼют не чіпаємо: старт лишається замʼютованим, просідання — ні
        default:  // Idle, Connecting, Error, Reconnecting
            if (!s_dacMuted) {
                muteDac(true, "state idle/error");
            }
            s_stableDone = false;
            break;
    }
}

// [USB7] Автомат файлового джерела (замість радіо-гілки updateState). Error / Idle для файлу — кінцеві стани
// до наступної команди: ніякого reconnect.
void updateFileState(uint32_t now) {
    if (s_state == PlayerState::Buffering || s_state == PlayerState::Playing) {
        trackSampleRate(now);  // [X1] та сама логіка розмʼюту XSMT за рядком "SampleRate (Hz)"
        monitorFile(now);
    }
    applyMutePolicy(now);
    // [USB7] Цифровий розмʼют — лише після відкриття ЦАП + kDigitalUnmuteDelayMs (soft-unmute PCM5102).
    if (s_fileActive && s_state == PlayerState::Playing && !s_dacMuted && !s_fileDigitalUnmuted) {
        if (!s_fileDacOpenSeen) {
            s_fileDacOpenSeen = true;
            s_fileDacOpenAtMs = now;
        } else if (elapsedAtLeast(now, s_fileDacOpenAtMs, static_cast<uint32_t>(s_fxDigDelayMs < 0 ? 0 : s_fxDigDelayMs))) {
            fileUnmuteRadioVolume();
            PLAYER_LOG("[PLAYER] file digital unmute (radio_volume)\n");
        }
    }
}

void updateState(uint32_t now) {
    if (s_source == Source::File) {  // [USB7] файлове джерело: радіо-логіка (reconnect, буфер, недовантаження) не діє
        updateFileState(now);
        return;
    }
    switch (s_state) {
        case PlayerState::Error:
            if (s_wantPlaying && timeReached(now, s_nextRetryMs)) {
                s_reconnectCount = s_reconnectCount + 1;
                connectNow(true);
            }
            break;
        case PlayerState::Buffering:
        case PlayerState::Playing:
            trackSampleRate(now);  // [X1]
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

bool AudioPlayer::begin() {  // [P48a] без параметра AudioProcessor
    if (s_task != nullptr) {
        return true;
    }

    // XSMT: ззовні pulldown, тож при живленні ЦАП замʼютований; явно тримаємо LOW.
    // [X1] Рівень LOW защолкується ДО увімкнення драйвера (як в AmpStandby::begin()): на піні немає імпульсу.
    // [X1b] gpio_set_level, а не digitalWrite: digitalWrite до pinMode дає в логу помилку "IO 45 is not set as GPIO".
    gpio_set_level(static_cast<gpio_num_t>(pins::kXsmt), player_cfg::kXsmtMutedLevel);
    pinMode(pins::kXsmt, OUTPUT);
    digitalWrite(pins::kXsmt, player_cfg::kXsmtMutedLevel);
    s_dacMuted = true;
    XSMT_EVT("player begin: XSMT LOW (push-pull output)");

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        Serial.println("[PLAYER] mutex create failed");
        return false;
    }
    s_station[0] = '\0';
    s_title[0] = '\0';
    s_pendingUrl[0] = '\0';
    s_url[0] = '\0';
    s_pendingFile[0] = '\0';  // [USB7]
    s_filePath[0] = '\0';

    // I2S ініціалізує бібліотека (конструктор + setPinout). DIN не чіпаємо.
    // [Prompt 37] MCLK (GPIO16, 256·fs) виводиться ЛИШЕ для АЦП PCM1808 (за LINE_PATH_ENABLE; P48d) (SCKI); PCM5102 його не
    // використовує (SCK на GND), тож на звук він не впливає. Без проходу (LINE_PATH_ENABLE 0) — як раніше.
    s_audio = new (std::nothrow) Audio();
    if (s_audio == nullptr) {
        Serial.println("[PLAYER] Audio object alloc failed");
        return false;
    }
#if LINE_PATH_ENABLE
    s_audio->setPinout(pins::kI2sBclk, pins::kI2sWs, pins::kI2sDout, pins::kI2sMclk);
#else
    s_audio->setPinout(pins::kI2sBclk, pins::kI2sWs, pins::kI2sDout);
#endif
#if AUDIO_PLAYER_HAS_SETBUFSIZE
    s_audio->setBufsize(player_cfg::kStreamBufRamBytes, player_cfg::kStreamBufPsramBytes);
#endif
    s_audio->setConnectionTimeout(player_cfg::kHttpTimeoutMs, player_cfg::kHttpsTimeoutMs);
    // Гучність бібліотеки — максимум і надалі не змінюється (гучність — цифрова, поза плеєром).
    s_audio->setVolume(player_cfg::kLibVolume);

    // [W1c] Внутрішня задача декодування бібліотеки за замовчуванням працює на ядрі 0 (Audio.h:
    // m_audioTaskCoreId = 0) і ділить його з UI, вебом і Wi-Fi -> затики при навантаженні ядра 0.
    // Спека (розділ 5): аудіо на ядрі 1. Викликаємо ПІСЛЯ setPinout() (там стартує задача).
    s_audio->setAudioTaskCore(player_cfg::kLibTaskCore);
    Serial.printf("[PLAYER] lib audio task core -> %u\n", static_cast<unsigned>(player_cfg::kLibTaskCore));

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

    Serial.printf("[PLAYER] ready (core %d, prio %u, buffer %s)\n",
                  player_cfg::kTaskCore, static_cast<unsigned>(player_cfg::kTaskPriority),
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
    XSMT_EVT("player playUrl accepted");
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
    XSMT_EVT("player stop accepted");
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

// [Prompt 44] Фіксація частоти I2S0 (див. audio_player.h).
bool AudioPlayer::setFixedOutputRate(bool fixed48k) {
    if (s_task == nullptr || s_state != PlayerState::Idle) {
        return false;
    }
    s_clkReq = fixed48k ? 1 : 2;
    return true;
}

bool AudioPlayer::fixedOutputRate() {
    return s_clkFixed;
}

// [Prompt X1]
bool AudioPlayer::dacUnmuted() {
    return !s_dacMuted;
}

// [Prompt X1] Лише рівень піна (атомарний запис регістра): безпечно з будь-якої задачі. s_dacMuted не чіпаємо
// (його веде задача плеєра); якщо плеєр потім розмʼютить, він зробить це за власною логікою.
void AudioPlayer::forceDacMute() {
    digitalWrite(pins::kXsmt, player_cfg::kXsmtMutedLevel);
    XSMT_EVT("player XSMT LOW (forceDacMute)");
}

void AudioPlayer::taskLoop() {
    for (;;) {
        processClockRequest();  // [Prompt 44]
        processCommands();
        processSeek();  // [USB7]
        processPause();  // [USB14]
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
        pollFilePosition(millis());  // [USB7] кеш позиції / тривалості файлу
        tagLogPoll(millis());        // [USB18a] лог ID3-тегів (із задачі плеєра)
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

// ===========================================================================
// [USB7] Файл: публічний API
// ===========================================================================

bool AudioPlayer::playFile(const char* path, int32_t startSec) {
    if (s_task == nullptr) {
        Serial.println("[PLAYER] playFile refused: player not started");
        return false;
    }
    if (path == nullptr || path[0] == '\0') {
        Serial.println("[PLAYER] playFile refused: empty path");
        return false;
    }
    if (strlen(path) >= file_player_cfg::kPathMax) {
        Serial.println("[PLAYER] playFile refused: path too long");
        return false;
    }
    if (startSec < -1) {
        startSec = -1;
    }
    const char* why = fileBlockReason();
    if (why != nullptr) {
        Serial.printf("[PLAYER] playFile refused: %s\n", why);
        return false;
    }
    Guard g;
    if (!g.ok()) {
        Serial.println("[PLAYER] playFile refused: player busy");
        return false;
    }
    if (s_cmd == Cmd::Play) {  // запит радіо ще не виконано
        Serial.println("[PLAYER] playFile refused: radio start is pending");
        return false;
    }
    strncpy(s_pendingFile, path, sizeof(s_pendingFile) - 1);
    s_pendingFile[sizeof(s_pendingFile) - 1] = '\0';
    s_pendingStartSec = startSec;
    s_cmd = Cmd::PlayFile;
    XSMT_EVT("player playFile accepted");
    return true;
}

bool AudioPlayer::isFileSource() { return s_source == Source::File; }

bool AudioPlayer::fileActive() { return s_fileActive; }

bool AudioPlayer::takeFileEnded() { return s_fileEnded.exchange(false); }

bool AudioPlayer::fileEndedPending() { return s_fileEnded.load(); }

uint32_t AudioPlayer::fileEndedAtMs() { return s_fileEndedAtMs; }

uint32_t AudioPlayer::fileDurationSec() { return s_fileDurSec; }

uint32_t AudioPlayer::filePositionSec() { return s_filePosSec; }

bool AudioPlayer::seekFileSec(int32_t sec, bool* atEnd) {
    if (atEnd != nullptr) {
        *atEnd = false;
    }
    if (s_task == nullptr || !s_fileActive) {
        return false;
    }
    // [USB18a] Єдине спільне місце рішення «seek за кінець»: за кешем позиції / тривалості (роздільність 1 с). sec > 0:
    // повернення назад біля кінця не завершує трек.
    const int64_t dur = static_cast<int64_t>(s_fileDurSec);
    const int64_t target = static_cast<int64_t>(s_filePosSec) + static_cast<int64_t>(sec);
    if (sec > 0 && dur > 0 && target >= dur - static_cast<int64_t>(file_player_cfg::kSeekEndMarginSec)) {
        s_seekSec = sec;
        s_seekPending = false;
        s_seekEndPending = true;
        if (atEnd != nullptr) {
            *atEnd = true;
        }
        return true;
    }
    s_seekEndPending = false;  // новіший звичайний seek замінює невиконаний «за кінець»
    s_seekSec = sec;
    s_seekPending = true;
    return true;
}

bool AudioPlayer::setFilePaused(bool paused) {
    if (s_task == nullptr) {
        return false;
    }
    if (s_source != Source::File || !s_fileActive) {
        Serial.println("[PLAYER] pause refused: no file is playing");
        return false;
    }
    if (s_fileEnded.load() || s_state != PlayerState::Playing) {
        Serial.printf("[PLAYER] pause refused: state %s\n", stateName(s_state));
        return false;
    }
    if (paused == s_filePaused) {
        return false;  // уже такий стан
    }
    s_pauseReq = paused ? 1 : 2;
    return true;
}

bool AudioPlayer::filePaused() { return s_filePaused; }

bool AudioPlayer::currentFilePath(char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = '\0';
    Guard g;
    if (!g.ok()) {
        return false;
    }
    copyUtf8Trunc(out, cap, s_filePath);
    return out[0] != '\0';
}

// [USB18a] Теги файлу path (див. audio_player.h).
bool AudioPlayer::fileTags(const char* path, FileTags* out) {
    if (path == nullptr || out == nullptr) {
        return false;
    }
    out->title[0] = '\0';
    out->artist[0] = '\0';
    out->album[0] = '\0';
    const uint32_t h = pathHash(path);
    bool mine = false;
    portENTER_CRITICAL(&s_tagMux);
    if (s_tags.pathHash == h) {
        memcpy(out->title, s_tags.title, sizeof(out->title));
        memcpy(out->artist, s_tags.artist, sizeof(out->artist));
        memcpy(out->album, s_tags.album, sizeof(out->album));
        mine = true;
    }
    portEXIT_CRITICAL(&s_tagMux);
    return mine;
}

const char* AudioPlayer::lastFileError() { return s_fileError; }

void AudioPlayer::setFileTune(uint32_t extraMs, int32_t digMs, int levelDb) {
    if (extraMs > file_player_cfg::kTuneMaxMs) {
        extraMs = file_player_cfg::kTuneMaxMs;
    }
    if (digMs > static_cast<int32_t>(file_player_cfg::kTuneMaxMs)) {
        digMs = static_cast<int32_t>(file_player_cfg::kTuneMaxMs);
    }
    if (digMs < -1) {
        digMs = -1;
    }
    if (levelDb < station_level_cfg::kLevelMinDb) {
        levelDb = station_level_cfg::kLevelMinDb;
    }
    if (levelDb > station_level_cfg::kLevelMaxDb) {
        levelDb = station_level_cfg::kLevelMaxDb;
    }
    s_fxExtraSettleMs = extraMs;
    s_fxDigDelayMs = digMs;
    s_fxLevelDb = levelDb;
}

void AudioPlayer::getFileTune(uint32_t* extraMs, int32_t* digMs, int* levelDb) {
    if (extraMs != nullptr) {
        *extraMs = s_fxExtraSettleMs;
    }
    if (digMs != nullptr) {
        *digMs = s_fxDigDelayMs;
    }
    if (levelDb != nullptr) {
        *levelDb = static_cast<int>(s_fxLevelDb);
    }
}

#if FEATURE_USB_HOST && USB_HOST_DIAG
// [DEF6] Лічильники кінців файлу для консолі `usb.eof`.
void AudioPlayer::fileEofStats(FileEofStats* out) {
    if (out == nullptr) {
        return;
    }
    out->real = s_eofRealCount;
    out->synth = s_eofSynthCount;
    out->stallNoAction = s_stallNoActionCount;
    out->watchdogOn = (FILE_EOF_WATCHDOG != 0);
    out->haveLast = s_lastStallValid;
    out->lastSynthesized = s_lastStallSynth;
    out->lastCur = s_lastStallCur;
    out->lastDur = s_lastStallDur;
    out->lastInbuf = s_lastStallInbuf;
    out->lastStalledMs = s_lastStallMs;
    memcpy(out->lastName, s_lastStallName, sizeof(out->lastName));
    out->lastName[sizeof(out->lastName) - 1] = '\0';
}

// [USB18a] Лічильники відхилених тегів і seek-ів за кінець.
void AudioPlayer::fileTagStats(uint32_t* rejected, uint32_t* seekEnd) {
    if (rejected != nullptr) {
        *rejected = s_tagRejected;
    }
    if (seekEnd != nullptr) {
        *seekEnd = s_seekEndCount;
    }
}
#endif
