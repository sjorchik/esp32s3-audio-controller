// StationStore: список станцій у LittleFS (JSON) + копія в PSRAM.
//
// Схема: живий список s_list[kMaxStations] у PSRAM. Читачі (get/count) беруть
// s_dataLock на мікросекунди. Імпорт/експорт серіалізуються s_ioLock; імпорт
// парсить у ТИМЧАСОВИЙ масив, пише файл (tmp + rename) і лише тоді під
// s_dataLock підміняє живий список — тому при будь-якій помилці старий список
// лишається цілим (атомарність). Порядок захоплення: s_ioLock -> s_dataLock,
// ніколи навпаки.
//
// [Prompt 14] Редагування (add/update/remove/move) іде тим самим шляхом, що й
// імпорт: під s_ioLock робимо ТИМЧАСОВУ копію списку, змінюємо її, пишемо файл
// і лише тоді під s_dataLock підміняємо живий список. Живий список не
// змінюється до успішного запису, тож «відкат» = відкинути копію.

#include "stations/station_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config/audio_player_config.h"  // лише kTestStation* для seed (не змінюється)
#include "config/station_store_config.h"

namespace cfg = station_store_cfg;

#if STATION_STORE_DEBUG
#define ST_LOG(...) Serial.printf("[STATIONS] " __VA_ARGS__)
#else
#define ST_LOG(...) \
    do {            \
    } while (0)
#endif

static_assert(sizeof(Station::name) == cfg::kNameMax, "Station.name size mismatch");
static_assert(sizeof(Station::url) == cfg::kUrlMax, "Station.url size mismatch");
static_assert(cfg::kUrlMax == player_cfg::kUrlMax, "must match player_cfg::kUrlMax");
static_assert(player_cfg::kTestStationCount >= 1 &&
                  player_cfg::kTestStationCount <= cfg::kMaxStations,
              "seed list must fit");

namespace {

// ---------------------------------------------------------------------------
// Стан
// ---------------------------------------------------------------------------
SemaphoreHandle_t s_dataLock = nullptr;  // захищає s_list/s_count (коротко)
SemaphoreHandle_t s_ioLock = nullptr;    // серіалізує імпорт/експорт
Station* s_list = nullptr;               // PSRAM
size_t s_count = 0;
bool s_ready = false;
const char* s_lastImportErr = "ok";  // [Prompt 14] пишеться під s_ioLock

enum class Res : uint8_t {
    Ok,
    NotFound,
    Empty,            // файл порожній або 0 валідних станцій
    TooBig,
    VersionMismatch,
    Invalid,          // не вдалося розібрати
    Overflow,         // більше kMaxStations
    IoError,
};

const char* resName(Res r) {
    switch (r) {
        case Res::Ok:              return "ok";
        case Res::NotFound:        return "file not found";
        case Res::Empty:           return "empty (no valid stations)";
        case Res::TooBig:          return "file too big";
        case Res::VersionMismatch: return "format version mismatch";
        case Res::Invalid:         return "parse error";
        case Res::Overflow:        return "too many stations";
        case Res::IoError:         return "I/O error";
    }
    return "?";
}

// [Prompt 14] Код причини для lastImportError() / HTTP-відповіді.
const char* resCode(Res r) {
    switch (r) {
        case Res::Ok:              return "ok";
        case Res::NotFound:        return "file_not_found";
        case Res::Empty:           return "empty";
        case Res::TooBig:          return "too_big";
        case Res::VersionMismatch: return "version_mismatch";
        case Res::Invalid:         return "parse_error";
        case Res::Overflow:        return "too_many_stations";
        case Res::IoError:         return "io_error";
    }
    return "io_error";
}

struct ParseStats {
    uint16_t invalid = 0;  // не http(s) URL
    uint16_t tooLong = 0;  // URL/рядок довший за буфер
};

// RAII для FreeRTOS-мʼютекса.
class Lock {
public:
    Lock(SemaphoreHandle_t m, TickType_t ticks)
        : m_(m), ok_(m != nullptr && xSemaphoreTake(m, ticks) == pdTRUE) {}
    ~Lock() {
        if (ok_) xSemaphoreGive(m_);
    }
    bool ok() const { return ok_; }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t m_;
    bool ok_;
};

// ---------------------------------------------------------------------------
// Памʼять: PSRAM, з відкатом на внутрішню RAM, якщо PSRAM немає
// ---------------------------------------------------------------------------
Station* allocStations(size_t n, bool zero) {
    const size_t bytes = n * sizeof(Station);
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (p != nullptr && zero) memset(p, 0, bytes);
    return static_cast<Station*>(p);
}

void freeStations(Station* p) {
    if (p != nullptr) heap_caps_free(p);
}

// Алокатор ArduinoJson у PSRAM: документ при розборі копіює всі рядки,
// не хочемо віддавати на це внутрішню RAM.
class PsramAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p != nullptr ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    void deallocate(void* ptr) override { heap_caps_free(ptr); }
    void* reallocate(void* ptr, size_t newSize) override {
        void* p = heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p != nullptr ? p : heap_caps_realloc(ptr, newSize, MALLOC_CAP_8BIT);
    }
};

// ---------------------------------------------------------------------------
// Рядки
// ---------------------------------------------------------------------------
// Повертає вказівник усередині s без пробілів з обох боків (змінює s).
char* trimInPlace(char* s) {
    while (*s != '\0' && isspace(static_cast<unsigned char>(*s))) ++s;
    size_t l = strlen(s);
    while (l > 0 && isspace(static_cast<unsigned char>(s[l - 1]))) s[--l] = '\0';
    return s;
}

// Те саме, але результат лишається на початку буфера.
void trimMove(char* s) {
    char* p = trimInPlace(s);
    if (p != s) memmove(s, p, strlen(p) + 1);
}

// Копія з обрізанням по межі UTF-8-символу.
void copyUtf8(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool isHttpUrl(const char* u) {
    return strncasecmp(u, "http://", 7) == 0 || strncasecmp(u, "https://", 8) == 0;
}

// Назва станції: з name; якщо порожня — URL без схеми; інакше "Station N".
void fillName(char* dst, const char* name, const char* url, size_t idx) {
    dst[0] = '\0';
    if (name != nullptr) {
        copyUtf8(dst, cfg::kNameMax, name);
        trimMove(dst);
    }
    if (dst[0] != '\0') return;
    const char* u = url;
    const char* sch = strstr(u, "://");
    if (sch != nullptr) u = sch + 3;
    if (*u != '\0') {
        copyUtf8(dst, cfg::kNameMax, u);
    } else {
        snprintf(dst, cfg::kNameMax, "Station %u", static_cast<unsigned>(idx + 1));
    }
}

enum class Add : uint8_t { Ok, Skipped, Overflow };

// Додає станцію в out[n] (n збільшується). Невалідні записи пропускає.
Add addStation(Station* out, size_t& n, const char* name, const char* url, ParseStats& st) {
    char u[cfg::kUrlMax];
    if (strlen(url) >= sizeof(u)) {
        ++st.tooLong;
        return Add::Skipped;
    }
    strcpy(u, url);
    trimMove(u);
    if (!isHttpUrl(u)) {
        ++st.invalid;
        return Add::Skipped;
    }
    if (n >= cfg::kMaxStations) return Add::Overflow;
    Station& s = out[n];
    memset(&s, 0, sizeof(s));
    s.id = static_cast<uint16_t>(n);
    strcpy(s.url, u);
    fillName(s.name, name, s.url, n);
    ++n;
    return Add::Ok;
}

// ---------------------------------------------------------------------------
// Файли
// ---------------------------------------------------------------------------
Res openRead(const char* path, File& f) {
    if (path == nullptr || path[0] != '/') return Res::IoError;
    if (!LittleFS.exists(path)) return Res::NotFound;
    f = LittleFS.open(path, "r");
    if (!f || f.isDirectory()) return Res::IoError;
    const size_t sz = f.size();
    if (sz == 0) return Res::Empty;
    if (sz > cfg::kMaxFileBytes) return Res::TooBig;
    return Res::Ok;
}

// Читає рядок без \r\n. false — EOF без жодного байта. truncated — рядок
// довший за cap-1 (залишок відкинуто).
bool readLine(File& f, char* buf, size_t cap, bool& truncated) {
    size_t n = 0;
    bool any = false;
    truncated = false;
    while (f.available()) {
        const int c = f.read();
        if (c < 0) break;
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < cap) {
            buf[n++] = static_cast<char>(c);
        } else {
            truncated = true;
        }
    }
    buf[n] = '\0';
    return any;
}

void stripBom(char* s) {
    if (static_cast<uint8_t>(s[0]) == 0xEF && static_cast<uint8_t>(s[1]) == 0xBB &&
        static_cast<uint8_t>(s[2]) == 0xBF) {
        memmove(s, s + 3, strlen(s + 3) + 1);
    }
}

// --- M3U -------------------------------------------------------------------
// Перша кома поза лапками в #EXTINF (атрибути tvg-* можуть містити коми в "").
const char* findTitleComma(const char* s) {
    bool inQuote = false;
    for (; *s != '\0'; ++s) {
        if (*s == '"') {
            inQuote = !inQuote;
        } else if (*s == ',' && !inQuote) {
            return s;
        }
    }
    return nullptr;
}

Res parseM3u(const char* path, Station* out, size_t& n, ParseStats& st) {
    File f;
    Res r = openRead(path, f);
    if (r != Res::Ok) return r;

    char line[cfg::kLineMax];
    char pending[cfg::kNameMax];
    bool havePending = false;
    bool first = true;
    bool trunc = false;
    n = 0;

    while (readLine(f, line, sizeof(line), trunc)) {
        if (first) {
            stripBom(line);
            first = false;
        }
        char* p = trimInPlace(line);
        if (*p == '\0') continue;

        if (*p == '#') {
            if (strncasecmp(p, "#EXTINF:", 8) == 0) {
                const char* comma = findTitleComma(p + 8);
                havePending = false;
                if (comma != nullptr) {
                    copyUtf8(pending, sizeof(pending), comma + 1);
                    trimMove(pending);
                    havePending = pending[0] != '\0';
                }
            }
            continue;
        }

        // Рядок з URL.
        const bool wasTruncated = trunc;
        const bool usePending = havePending;
        havePending = false;
        if (wasTruncated) {
            ++st.tooLong;
            continue;
        }
        if (addStation(out, n, usePending ? pending : nullptr, p, st) == Add::Overflow) {
            return Res::Overflow;
        }
    }
    return n > 0 ? Res::Ok : Res::Empty;
}

// --- PLS -------------------------------------------------------------------
bool allDigits(const char* s) {
    if (*s == '\0') return false;
    for (; *s != '\0'; ++s) {
        if (!isdigit(static_cast<unsigned char>(*s))) return false;
    }
    return true;
}

// out — масив kMaxStations, обнулений: запис FileN/TitleN лягає в out[N-1],
// наприкінці порожні пропуски вибираються (компактизація на місці).
Res parsePls(const char* path, Station* out, size_t& n, ParseStats& st) {
    File f;
    Res r = openRead(path, f);
    if (r != Res::Ok) return r;

    char line[cfg::kLineMax];
    bool trunc = false;
    n = 0;

    while (readLine(f, line, sizeof(line), trunc)) {
        char* p = trimInPlace(line);
        if (*p == '\0' || *p == ';' || *p == '#' || *p == '[') continue;
        char* eq = strchr(p, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        char* key = trimInPlace(p);
        char* val = trimInPlace(eq + 1);

        if (strcasecmp(key, "NumberOfEntries") == 0) {
            if (strtoul(val, nullptr, 10) > cfg::kMaxStations) return Res::Overflow;
        } else if (strncasecmp(key, "File", 4) == 0 && allDigits(key + 4)) {
            const unsigned long idx = strtoul(key + 4, nullptr, 10);
            if (idx == 0) continue;
            if (trunc || strlen(val) >= cfg::kUrlMax) {
                ++st.tooLong;
                continue;
            }
            if (!isHttpUrl(val)) {
                ++st.invalid;
                continue;
            }
            if (idx > cfg::kMaxStations) return Res::Overflow;
            strcpy(out[idx - 1].url, val);
        } else if (strncasecmp(key, "Title", 5) == 0 && allDigits(key + 5)) {
            const unsigned long idx = strtoul(key + 5, nullptr, 10);
            if (idx == 0 || idx > cfg::kMaxStations) continue;  // без FileN марний
            copyUtf8(out[idx - 1].name, cfg::kNameMax, val);
        }
    }

    size_t w = 0;
    for (size_t i = 0; i < cfg::kMaxStations; ++i) {
        if (out[i].url[0] == '\0') continue;
        if (w != i) out[w] = out[i];
        char nm[cfg::kNameMax];
        strlcpy(nm, out[w].name, sizeof(nm));
        fillName(out[w].name, nm, out[w].url, w);
        out[w].id = static_cast<uint16_t>(w);
        ++w;
    }
    n = w;
    return n > 0 ? Res::Ok : Res::Empty;
}

// --- JSON ------------------------------------------------------------------
// requireVersion=true (файл сховища): без "version" — VersionMismatch.
// requireVersion=false (імпорт користувача): допускається голий масив
// [{"name","url"},...]; якщо "version" є — має збігатися.
Res parseJson(const char* path, bool requireVersion, Station* out, size_t& n, ParseStats& st) {
    File f;
    Res r = openRead(path, f);
    if (r != Res::Ok) return r;

    PsramAllocator alloc;
    JsonDocument doc(&alloc);
    const DeserializationError err = deserializeJson(doc, f);
    if (err) {
        ST_LOG("JSON error: %s\n", err.c_str());
        return err == DeserializationError::NoMemory ? Res::IoError : Res::Invalid;
    }

    JsonArrayConst arr;
    if (doc.is<JsonObject>()) {
        JsonVariantConst ver = doc["version"];
        if (ver.isNull()) {
            if (requireVersion) {
                ST_LOG("WARNING: no \"version\" field\n");
                return Res::VersionMismatch;
            }
        } else if (!ver.is<uint32_t>() || ver.as<uint32_t>() != cfg::kFormatVersion) {
            ST_LOG("WARNING: format version %s, expected %u\n", ver.as<String>().c_str(),
                   static_cast<unsigned>(cfg::kFormatVersion));
            return Res::VersionMismatch;
        }
        arr = doc["stations"].as<JsonArrayConst>();
    } else if (doc.is<JsonArray>() && !requireVersion) {
        arr = doc.as<JsonArrayConst>();
    } else {
        return Res::Invalid;
    }
    if (arr.isNull()) return Res::Invalid;

    n = 0;
    for (JsonVariantConst item : arr) {
        const char* name = item["name"] | "";
        const char* url = item["url"] | "";
        if (addStation(out, n, name, url, st) == Add::Overflow) return Res::Overflow;
    }
    return n > 0 ? Res::Ok : Res::Empty;
}

// --- Запис JSON ------------------------------------------------------------
// Потоковий запис без проміжного JsonDocument (не дублюємо всі рядки в памʼяті).
class JsonOut {
public:
    explicit JsonOut(File& f) : f_(f) {}
    void put(char c) {
        if (len_ == sizeof(buf_)) flush();
        buf_[len_++] = c;
    }
    void puts(const char* s) {
        while (*s != '\0') put(*s++);
    }
    void str(const char* s) {
        put('"');
        for (; *s != '\0'; ++s) {
            const uint8_t c = static_cast<uint8_t>(*s);
            if (c == '"' || c == '\\') {
                put('\\');
                put(static_cast<char>(c));
            } else if (c < 0x20) {
                char e[8];
                snprintf(e, sizeof(e), "\\u%04x", c);
                puts(e);
            } else {
                put(static_cast<char>(c));  // UTF-8 байти — як є
            }
        }
        put('"');
    }
    void flush() {
        if (len_ > 0) {
            if (f_.write(reinterpret_cast<const uint8_t*>(buf_), len_) != len_) ok_ = false;
            len_ = 0;
        }
    }
    bool ok() const { return ok_; }

private:
    File& f_;
    char buf_[256];
    size_t len_ = 0;
    bool ok_ = true;
};

// Атомарний запис: path.tmp -> rename(path). Якщо rename поверх існуючого
// не пройшов — видаляємо ціль і пробуємо ще раз (це вікно НЕ атомарне).
bool writeJsonFile(const char* path, const Station* list, size_t n) {
    if (path == nullptr || path[0] != '/') return false;
    if (LittleFS.totalBytes() == 0) return false;  // LittleFS не змонтовано

    char tmp[96];
    if (snprintf(tmp, sizeof(tmp), "%s%s", path, cfg::kTmpSuffix) >= static_cast<int>(sizeof(tmp))) {
        return false;
    }
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;

    JsonOut o(f);
    char head[48];
    snprintf(head, sizeof(head), "{\"version\":%u,\"stations\":[\n",
             static_cast<unsigned>(cfg::kFormatVersion));
    o.puts(head);
    for (size_t i = 0; i < n; ++i) {
        o.puts("{\"name\":");
        o.str(list[i].name);
        o.puts(",\"url\":");
        o.str(list[i].url);
        o.puts(i + 1 < n ? "},\n" : "}\n");
    }
    o.puts("]}\n");
    o.flush();
    f.close();

    if (!o.ok()) {
        LittleFS.remove(tmp);
        return false;
    }
    if (!LittleFS.rename(tmp, path)) {
        LittleFS.remove(path);
        if (!LittleFS.rename(tmp, path)) {
            LittleFS.remove(tmp);
            return false;
        }
    }
    return true;
}

void backupBadStorage() {
    char bad[96];
    if (snprintf(bad, sizeof(bad), "%s%s", cfg::kStoragePath, cfg::kBackupSuffix) >=
        static_cast<int>(sizeof(bad))) {
        return;
    }
    LittleFS.remove(bad);
    if (LittleFS.rename(cfg::kStoragePath, bad)) {
        ST_LOG("old storage file kept as %s\n", bad);
    }
}

// Вбудований початковий список (з player_cfg::kTestStation*).
size_t fillSeed(Station* list) {
    size_t n = 0;
    ParseStats st;
    for (size_t i = 0; i < player_cfg::kTestStationCount; ++i) {
        addStation(list, n, player_cfg::kTestStationNames[i], player_cfg::kTestStationUrls[i], st);
    }
    return n;
}

enum class Fmt : uint8_t { M3u, Pls, Json };

bool importCommon(const char* path, Fmt fmt) {
    if (!s_ready || path == nullptr) return false;

    Lock io(s_ioLock, pdMS_TO_TICKS(cfg::kIoMutexTimeoutMs));
    if (!io.ok()) {
        ST_LOG("import: busy\n");
        s_lastImportErr = "busy";  // без мʼютекса: лише інформативне значення
        return false;
    }

    Station* tmp = allocStations(cfg::kMaxStations, true);
    if (tmp == nullptr) {
        ST_LOG("import: out of memory\n");
        s_lastImportErr = "out_of_memory";
        return false;
    }

    size_t n = 0;
    ParseStats st;
    Res r = Res::Invalid;
    switch (fmt) {
        case Fmt::M3u:  r = parseM3u(path, tmp, n, st); break;
        case Fmt::Pls:  r = parsePls(path, tmp, n, st); break;
        case Fmt::Json: r = parseJson(path, false, tmp, n, st); break;
    }
    if (r != Res::Ok) {
        ST_LOG("import %s failed: %s (list unchanged)\n", path, resName(r));
        s_lastImportErr = resCode(r);
        freeStations(tmp);
        return false;
    }
    for (size_t i = 0; i < n; ++i) tmp[i].id = static_cast<uint16_t>(i);

    // Спершу на диск: якщо запис не вдався — RAM і диск лишаються узгодженими
    // (обидва зі старим списком).
    if (!writeJsonFile(cfg::kStoragePath, tmp, n)) {
        ST_LOG("import %s: storage write failed (list unchanged)\n", path);
        s_lastImportErr = "storage_write_failed";
        freeStations(tmp);
        return false;
    }

    {
        Lock data(s_dataLock, portMAX_DELAY);  // утримання — лише memcpy
        memcpy(s_list, tmp, n * sizeof(Station));
        s_count = n;
    }
    freeStations(tmp);
    s_lastImportErr = "ok";
    ST_LOG("import %s ok: %u stations (skipped: %u not http(s), %u too long)\n", path,
           static_cast<unsigned>(n), static_cast<unsigned>(st.invalid),
           static_cast<unsigned>(st.tooLong));
    return true;
}

// ---------------------------------------------------------------------------
// [Prompt 14] Редагування
// ---------------------------------------------------------------------------
// Валідація й нормалізація станції від викликача (критерій як в імпорті):
// url не довший за буфер (обрізати URL не можна — він марний), без пробілів по
// краях, http(s)://; name обрізається по межі UTF-8 до kNameMax-1 і не порожнє.
bool normalizeStation(const Station& in, Station& out) {
    if (strnlen(in.url, sizeof(in.url)) >= sizeof(in.url)) return false;
    char u[cfg::kUrlMax];
    strcpy(u, in.url);
    trimMove(u);
    if (!isHttpUrl(u)) return false;

    char nm[cfg::kNameMax + 1];  // name міг прийти без '\0'
    memcpy(nm, in.name, cfg::kNameMax);
    nm[cfg::kNameMax] = '\0';

    memset(&out, 0, sizeof(out));
    copyUtf8(out.name, cfg::kNameMax, nm);
    trimMove(out.name);
    if (out.name[0] == '\0') return false;
    strcpy(out.url, u);
    return true;
}

// Сеанс редагування: s_ioLock + робоча копія списку (PSRAM).
struct EditSession {
    Lock io;
    Station* work = nullptr;
    size_t n = 0;

    EditSession() : io(s_ioLock, pdMS_TO_TICKS(cfg::kEditIoMutexTimeoutMs)) {}
    ~EditSession() { freeStations(work); }
    EditSession(const EditSession&) = delete;
    EditSession& operator=(const EditSession&) = delete;

    bool open() {
        if (!s_ready || !io.ok()) return false;
        work = allocStations(cfg::kMaxStations, false);
        if (work == nullptr) return false;
        Lock data(s_dataLock, portMAX_DELAY);  // утримання — лише memcpy
        n = s_count;
        memcpy(work, s_list, n * sizeof(Station));
        return true;
    }

    // Записує копію у файл; лише після успіху підміняє живий список.
    // false — файл не записано, живий список НЕ змінено.
    bool commit() {
        for (size_t i = 0; i < n; ++i) work[i].id = static_cast<uint16_t>(i);
        if (!writeJsonFile(cfg::kStoragePath, work, n)) return false;
        Lock data(s_dataLock, portMAX_DELAY);
        memcpy(s_list, work, n * sizeof(Station));
        if (n < s_count) memset(s_list + n, 0, (s_count - n) * sizeof(Station));
        s_count = n;
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Публічний API
// ---------------------------------------------------------------------------
bool StationStore::begin() {
    if (s_ready) return true;  // виклик з одного потоку на старті (AppController::begin)

    s_dataLock = xSemaphoreCreateMutex();
    s_ioLock = xSemaphoreCreateMutex();
    if (s_dataLock == nullptr || s_ioLock == nullptr) {
        ST_LOG("mutex create failed\n");
        return false;
    }
    s_list = allocStations(cfg::kMaxStations, true);
    if (s_list == nullptr) {
        ST_LOG("out of memory for station list\n");
        return false;
    }

    const bool mounted = LittleFS.totalBytes() > 0;
    if (!mounted) {
        ST_LOG("WARNING: LittleFS not mounted, list is RAM-only\n");
    }

    size_t n = 0;
    ParseStats st;
    const Res r = mounted ? parseJson(cfg::kStoragePath, true, s_list, n, st) : Res::IoError;
    if (r == Res::Ok) {
        s_count = n;
        ST_LOG("loaded %u stations from %s\n", static_cast<unsigned>(n), cfg::kStoragePath);
    } else {
        ST_LOG("%s: %s -> seeding built-in list\n", cfg::kStoragePath, resName(r));
        if (mounted && (r == Res::Invalid || r == Res::VersionMismatch || r == Res::Overflow ||
                        r == Res::TooBig)) {
            backupBadStorage();
        }
        memset(s_list, 0, cfg::kMaxStations * sizeof(Station));
        s_count = fillSeed(s_list);
        if (mounted && !writeJsonFile(cfg::kStoragePath, s_list, s_count)) {
            ST_LOG("WARNING: seed could not be written to flash\n");
        }
    }

    s_ready = true;
    ST_LOG("ready: %u stations (%u bytes in %s)\n", static_cast<unsigned>(s_count),
           static_cast<unsigned>(cfg::kMaxStations * sizeof(Station)),
           esp_ptr_external_ram(s_list) ? "PSRAM" : "internal RAM");
    return s_count > 0;
}

size_t StationStore::count() {
    if (!s_ready) return 0;
    Lock l(s_dataLock, pdMS_TO_TICKS(cfg::kMutexTimeoutMs));
    return l.ok() ? s_count : 0;
}

bool StationStore::get(size_t index, Station& out) {
    if (!s_ready) return false;
    Lock l(s_dataLock, pdMS_TO_TICKS(cfg::kMutexTimeoutMs));
    if (!l.ok() || index >= s_count) return false;
    out = s_list[index];
    out.id = static_cast<uint16_t>(index);
    return true;
}

bool StationStore::importM3u(const char* path) { return importCommon(path, Fmt::M3u); }
bool StationStore::importPls(const char* path) { return importCommon(path, Fmt::Pls); }
bool StationStore::importJson(const char* path) { return importCommon(path, Fmt::Json); }

const char* StationStore::lastImportError() { return s_lastImportErr; }

bool StationStore::add(const Station& st, size_t* outIndex) {
    Station norm;
    if (!normalizeStation(st, norm)) {
        ST_LOG("add: invalid station\n");
        return false;
    }
    EditSession ed;
    if (!ed.open()) {
        ST_LOG("add: busy or out of memory\n");
        return false;
    }
    if (ed.n >= cfg::kMaxStations) {
        ST_LOG("add: list full\n");
        return false;
    }
    ed.work[ed.n++] = norm;
    if (!ed.commit()) {
        ST_LOG("add: storage write failed (list unchanged)\n");
        return false;
    }
    if (outIndex != nullptr) *outIndex = ed.n - 1;
    ST_LOG("add ok: #%u (%u total)\n", static_cast<unsigned>(ed.n - 1), static_cast<unsigned>(ed.n));
    return true;
}

bool StationStore::update(size_t index, const Station& st) {
    Station norm;
    if (!normalizeStation(st, norm)) {
        ST_LOG("update: invalid station\n");
        return false;
    }
    EditSession ed;
    if (!ed.open()) {
        ST_LOG("update: busy or out of memory\n");
        return false;
    }
    if (index >= ed.n) return false;
    ed.work[index] = norm;
    if (!ed.commit()) {
        ST_LOG("update #%u: storage write failed (list unchanged)\n", static_cast<unsigned>(index));
        return false;
    }
    ST_LOG("update ok: #%u\n", static_cast<unsigned>(index));
    return true;
}

bool StationStore::remove(size_t index) {
    EditSession ed;
    if (!ed.open()) {
        ST_LOG("remove: busy or out of memory\n");
        return false;
    }
    if (index >= ed.n) return false;
    memmove(ed.work + index, ed.work + index + 1, (ed.n - index - 1) * sizeof(Station));
    --ed.n;
    if (!ed.commit()) {
        ST_LOG("remove #%u: storage write failed (list unchanged)\n", static_cast<unsigned>(index));
        return false;
    }
    ST_LOG("remove ok: #%u (%u left)\n", static_cast<unsigned>(index), static_cast<unsigned>(ed.n));
    return true;
}

bool StationStore::move(size_t fromIndex, size_t toIndex) {
    EditSession ed;
    if (!ed.open()) {
        ST_LOG("move: busy or out of memory\n");
        return false;
    }
    if (fromIndex >= ed.n || toIndex >= ed.n) return false;
    if (fromIndex == toIndex) return true;  // нічого змінювати й писати
    const Station moving = ed.work[fromIndex];
    if (fromIndex < toIndex) {
        memmove(ed.work + fromIndex, ed.work + fromIndex + 1, (toIndex - fromIndex) * sizeof(Station));
    } else {
        memmove(ed.work + toIndex + 1, ed.work + toIndex, (fromIndex - toIndex) * sizeof(Station));
    }
    ed.work[toIndex] = moving;
    if (!ed.commit()) {
        ST_LOG("move %u->%u: storage write failed (list unchanged)\n",
               static_cast<unsigned>(fromIndex), static_cast<unsigned>(toIndex));
        return false;
    }
    ST_LOG("move ok: %u -> %u\n", static_cast<unsigned>(fromIndex), static_cast<unsigned>(toIndex));
    return true;
}

bool StationStore::exportJson(const char* path) {
    if (!s_ready || path == nullptr) return false;

    Lock io(s_ioLock, pdMS_TO_TICKS(cfg::kIoMutexTimeoutMs));
    if (!io.ok()) return false;

    // Знімок списку: файл пишеться довго, а мʼютекс даних має триматись коротко.
    Station* copy = nullptr;
    size_t n = 0;
    {
        Lock data(s_dataLock, portMAX_DELAY);
        n = s_count;
        if (n > 0) {
            copy = allocStations(n, false);
            if (copy == nullptr) return false;
            memcpy(copy, s_list, n * sizeof(Station));
        }
    }
    const bool ok = writeJsonFile(path, copy, n);
    freeStations(copy);
    if (!ok) ST_LOG("export to %s failed\n", path);
    return ok;
}
