#pragma once

// [USB17a] Чиста (без Arduino / IDF) нормалізація тексту тегів / імені файла під шрифти дисплея. Один набір правил на
// весь проєкт для екрана «зараз грає»; працює при копіюванні у фіксовані буфери (НЕ в кадрі малювання).
//
// Що малює шрифт (звірено за таблицею гліфів ui/font_data.h, усі розміри мають діапазон 0x0020..0x0491, непорожні
// гліфи): 0x20..0x7E, 0xA1..0xAC, 0xAE..0x17F, 0x400..0x45F, 0x490..0x491. Решта кодів у таблиці — порожні гліфи.
//
// Правила:
//  - символ зі шрифту — копіюється як є;
//  - типографські: тире / дефіси U+2010..U+2015 і мінус U+2212 -> '-'; ' ’ ‚ ‛ ′ -> '\''; “ ” „ ‟ ″ -> '"'; … -> "...";
//  - керівні символи (<0x20, 0x7F..0x9F) і нерозривний пробіл U+00A0 -> пробіл; «нульової ширини» (U+00AD, U+200B..U+200D,
//    U+FEFF) -> пропуск;
//  - усе інше (CJK, емодзі, ...) і недійсний UTF-8 -> '?';
//  - кілька пробілів поспіль стискаються в один, пробіли на краях зрізаються;
//  - результат усікається по межі символу, щоб вміститись у cap (разом з '\0').
// cp1251 / UTF-16 не перекодовуються (такі теги відкидає плеєр ще до цього).

#include <stddef.h>
#include <stdint.h>

namespace usb_np_text {

// Декодує один символ UTF-8. Повертає довжину (>= 1); недійсна послідовність -> *cp = 0xFFFFFFFF, довжина 1.
inline size_t decode(const uint8_t* p, uint32_t* cp) {
    const uint8_t c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    size_t n = 0;
    uint32_t v = 0;
    uint32_t minV = 0;
    if ((c & 0xE0) == 0xC0)      { n = 2; v = c & 0x1F; minV = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; minV = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; minV = 0x10000; }
    else { *cp = 0xFFFFFFFFu; return 1; }
    for (size_t i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFFFFFFu; return 1; }   // p[i] == 0 теж сюди (кінець рядка)
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < minV || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFFFFFFu; return 1; }
    *cp = v;
    return n;
}

// true — шрифт має непорожній гліф для цього коду.
inline bool glyphOk(uint32_t cp) {
    return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA1 && cp <= 0xAC) || (cp >= 0xAE && cp <= 0x17F) ||
           (cp >= 0x400 && cp <= 0x45F) || cp == 0x490 || cp == 0x491;
}

// Нормалізує перші srcLen байтів src (або до '\0') у out (cap >= 1, разом з '\0'). Повертає довжину результату в байтах.
inline size_t sanitizeN(const char* src, size_t srcLen, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    if (src == nullptr) return 0;
    size_t len = 0;
    bool pendingSpace = false;
    const uint8_t* const base = reinterpret_cast<const uint8_t*>(src);
    size_t pos = 0;
    while (pos < srcLen && base[pos] != 0) {
        const uint8_t* const start = base + pos;
        uint32_t cp = 0;
        const size_t n = decode(start, &cp);
        const char* rep = nullptr;     // заміна ASCII-рядком
        bool copy = false;             // копіювати n байтів оригіналу
        bool isSpace = false;
        bool skip = false;
        if (cp == 0xFFFFFFFFu)                                   { rep = "?"; }
        else if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xA0 || cp == 0x20) { isSpace = true; }
        else if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200D) || cp == 0xFEFF) { skip = true; }
        else if ((cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212) { rep = "-"; }
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x201B || cp == 0x2032) { rep = "'"; }
        else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x201F || cp == 0x2033) { rep = "\""; }
        else if (cp == 0x2026)                                   { rep = "..."; }
        else if (glyphOk(cp))                                    { copy = true; }
        else                                                     { rep = "?"; }
        pos += n;
        if (skip) continue;
        if (isSpace) { pendingSpace = (len > 0); continue; }
        size_t need = 0;
        if (copy) { need = n; }
        else      { while (rep[need] != '\0') ++need; }
        const size_t extra = pendingSpace ? 1 : 0;
        if (len + extra + need + 1 > cap) break;                 // не влізло — усікаємо по межі символу
        if (pendingSpace) { out[len++] = ' '; pendingSpace = false; }
        if (copy) { for (size_t i = 0; i < need; ++i) out[len++] = static_cast<char>(start[i]); }
        else      { for (size_t i = 0; i < need; ++i) out[len++] = rep[i]; }
    }
    out[len] = '\0';
    return len;
}

inline size_t sanitize(const char* src, char* out, size_t cap) {
    return sanitizeN(src, static_cast<size_t>(-1), out, cap);
}

// Імʼя файла без каталогу й без останнього розширення (".mp3"), нормалізоване sanitize().
inline size_t displayNameFromPath(const char* path, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    if (path == nullptr) return 0;
    const char* base = path;
    for (const char* q = path; *q != '\0'; ++q) {
        if (*q == '/') base = q + 1;
    }
    size_t blen = 0;
    while (base[blen] != '\0') ++blen;
    size_t dot = blen;
    for (size_t i = blen; i > 1; --i) {                           // i > 1: ".hidden" (крапка на початку) не розширення
        if (base[i - 1] == '.') { dot = i - 1; break; }
    }
    return sanitizeN(base, dot, out, cap);
}

}  // namespace usb_np_text
