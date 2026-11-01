// [USB6] fs::FS-обгортка над VFS-монтуванням USB-флешки (/usb) + діагностика читання (usb.fs.*).
//
// Архітектура:
//  - UsbVolumeImpl : VFSImpl. VFSImpl сам додає до шляху свою точку монтування (_mountpoint), тому
//    обгортка лише задає _mountpoint = kMountPath і перевіряє стан тому перед кожною операцією.
//  - UsbFileImpl : FileImpl. Обгортка над VFSFileImpl. Кожен виклик іде під коротким блокуванням
//    тому (usb_msc_mount::volumeLock) і перевіряє, що файл відкрито в ПОТОЧНОМУ монтуванні
//    (покоління, usb_msc_mount::volumeGenerationLocked). Після розмонтування або повторного
//    монтування файл «осиротілий»: VFS-виклики більше не робляться (див. «Відомі обмеження»).
//  - Лише читання: режими запису відхиляються в open(); write() повертає 0.
//
// Вкладених блокувань немає: мьютекс не рекурсивний, тож жоден метод обгортки не викликає інший метод,
// що бере блокування, поки тримає власне.

#include "usb/usb_fs.h"

#if FEATURE_USB_HOST

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <memory>

#include "config/usb_host_config.h"
#include "esp_heap_caps.h"
#include "usb/usb_msc_mount.h"
#include "vfs_api.h"  // VFSImpl / VFSFileImpl з бібліотеки FS (ядро Arduino-ESP32 3.3.7)

namespace usb_fs {

namespace {

// Шлях: з '/' на початку, без "..", з запасом у буфері kPathMax (з префіксом точки монтування).
bool pathOk(const char* path) {
    if (path == nullptr || path[0] != '/') {
        return false;
    }
    if (strstr(path, "..") != nullptr) {
        return false;
    }
    return strlen(path) + strlen(usb_host_cfg::kMountPath) < usb_host_cfg::kPathMax;
}

// Лише читання: відкриваємо тільки за замовчуванням або з режимом, що починається з 'r'.
bool isReadMode(const char* mode) {
    return mode == nullptr || mode[0] == 'r';
}

// Блокування тому на одну операцію. Конструктор бере блокування (не довше kFsLockWaitMs),
// деструктор знімає. ready() — том змонтовано й блокування взято; sameMount(gen) — це те саме монтування.
class VolumeGuard {
public:
    VolumeGuard() : _locked(usb_msc_mount::volumeLock(usb_host_cfg::kFsLockWaitMs)) {
        if (_locked) {
            _gen = usb_msc_mount::volumeGenerationLocked();
        }
    }
    ~VolumeGuard() {
        if (_locked) {
            usb_msc_mount::volumeUnlock();
        }
    }
    VolumeGuard(const VolumeGuard&) = delete;
    VolumeGuard& operator=(const VolumeGuard&) = delete;

    bool ready() const { return _gen != 0; }
    bool sameMount(uint32_t gen) const { return _gen != 0 && _gen == gen; }
    uint32_t generation() const { return _gen; }

private:
    bool _locked;
    uint32_t _gen = 0;
};

// Обгортка над файлом VFS. Усі методи, крім деструктора, беруть блокування тому й перевіряють покоління.
class UsbFileImpl : public fs::FileImpl {
public:
    UsbFileImpl(fs::FileImplPtr inner, uint32_t gen) : _inner(inner), _gen(gen) {}
    ~UsbFileImpl() override { close(); }

    // Лише читання: запис відхилено.
    size_t write(const uint8_t* buf, size_t size) override {
        (void)buf;
        (void)size;
        return 0;
    }

    size_t read(uint8_t* buf, size_t size) override {
        VolumeGuard g;
        if (!g.sameMount(_gen)) {
            return 0;
        }
        return _inner->read(buf, size);
    }

    void flush() override {}

    bool seek(uint32_t pos, fs::SeekMode mode) override {
        VolumeGuard g;
        return g.sameMount(_gen) && _inner->seek(pos, mode);
    }

    size_t position() const override {
        VolumeGuard g;
        return g.sameMount(_gen) ? _inner->position() : 0;
    }

    size_t size() const override {
        VolumeGuard g;
        return g.sameMount(_gen) ? _inner->size() : 0;
    }

    bool setBufferSize(size_t size) override {
        VolumeGuard g;
        return g.sameMount(_gen) && _inner->setBufferSize(size);
    }

    void close() override {
        if (!_inner) {
            return;
        }
        VolumeGuard g;
        if (g.sameMount(_gen)) {
            _inner->close();
            _inner.reset();  // ~VFSFileImpl → fclose(), поки том змонтовано (блокування тримаємо)
        } else {
            // Том знято або змінено після відкриття, або блокування не взято за kFsLockWaitMs.
            // VFS-обʼєкт торкати не можна: fclose() на знятому томі небезпечний. Навмисний витік:
            // тримаємо копію shared_ptr назавжди. Див. «Відомі обмеження».
            new fs::FileImplPtr(_inner);
            _inner.reset();
        }
    }

    time_t getLastWrite() override {
        VolumeGuard g;
        return g.sameMount(_gen) ? _inner->getLastWrite() : 0;
    }

    const char* path() const override {
        VolumeGuard g;
        return g.sameMount(_gen) ? _inner->path() : nullptr;
    }

    const char* name() const override {
        VolumeGuard g;
        return g.sameMount(_gen) ? _inner->name() : nullptr;
    }

    boolean isDirectory(void) override {
        VolumeGuard g;
        return g.sameMount(_gen) && _inner->isDirectory();
    }

    fs::FileImplPtr openNextFile(const char* mode) override {
        if (!isReadMode(mode)) {
            return fs::FileImplPtr();
        }
        VolumeGuard g;
        if (!g.sameMount(_gen)) {
            return fs::FileImplPtr();
        }
        fs::FileImplPtr child = _inner->openNextFile("r");
        if (!child || !*child) {
            return fs::FileImplPtr();
        }
        return std::make_shared<UsbFileImpl>(child, _gen);
    }

    boolean seekDir(long position) override {
        VolumeGuard g;
        return g.sameMount(_gen) && _inner->seekDir(position);
    }

    String getNextFileName(void) override {
        VolumeGuard g;
        return g.sameMount(_gen) ? _inner->getNextFileName() : String();
    }

    String getNextFileName(bool* isDir) override {
        VolumeGuard g;
        if (!g.sameMount(_gen)) {
            if (isDir != nullptr) {
                *isDir = false;
            }
            return String();
        }
        return _inner->getNextFileName(isDir);
    }

    void rewindDirectory(void) override {
        VolumeGuard g;
        if (g.sameMount(_gen)) {
            _inner->rewindDirectory();
        }
    }

    // File::operator bool() перевіряє це перед кожною операцією, тож осиротілий файл стає «порожнім».
    operator bool() override {
        VolumeGuard g;
        return g.sameMount(_gen) && _inner && static_cast<bool>(*_inner);
    }

private:
    fs::FileImplPtr _inner;
    uint32_t _gen;
};

// Том /usb. Наслідує VFSImpl (його open / exists працюють з _mountpoint), але кожен виклик спершу бере
// блокування тому й відмовляє, коли том не змонтовано. Лише читання.
class UsbVolumeImpl : public VFSImpl {
public:
    UsbVolumeImpl() { _mountpoint = usb_host_cfg::kMountPath; }

    fs::FileImplPtr open(const char* path, const char* mode, const bool create) override {
        (void)create;
        if (!pathOk(path) || !isReadMode(mode)) {
            return fs::FileImplPtr();
        }
        VolumeGuard g;
        if (!g.ready()) {
            return fs::FileImplPtr();
        }
        fs::FileImplPtr inner = VFSImpl::open(path, "r", false);
        if (!inner || !*inner) {
            // Немає файлу / каталогу, або fopen() відмовив (напр., вичерпано max_files).
            return fs::FileImplPtr();
        }
        return std::make_shared<UsbFileImpl>(inner, g.generation());
    }

    bool exists(const char* path) override {
        if (!pathOk(path)) {
            return false;
        }
        VolumeGuard g;
        return g.ready() && VFSImpl::exists(path);
    }

    // Лише читання: зміни файлової системи відхилено.
    bool rename(const char* pathFrom, const char* pathTo) override {
        (void)pathFrom;
        (void)pathTo;
        return false;
    }
    bool remove(const char* path) override {
        (void)path;
        return false;
    }
    bool mkdir(const char* path) override {
        (void)path;
        return false;
    }
    bool rmdir(const char* path) override {
        (void)path;
        return false;
    }
};

}  // namespace

fs::FS& volume() {
    static fs::FS s_volume(std::make_shared<UsbVolumeImpl>());
    return s_volume;
}

#if USB_HOST_DIAG

namespace {

constexpr const char* kTag = "[USB]";

// CRC32 (IEEE 802.3 / zlib), побітовий: без таблиці, щоб не тримати 1 КБ RAM. Інкрементальний.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// Слово команди. Повертає true, якщо рядок починається з word і далі пробіл або кінець;
// *rest — хвіст після пробілів.
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

// Розбір аргументів: шлях може містити пробіли (кирилиця, «Папка 1/пісня.mp3»), тож шлях — весь хвіст
// рядка без пробілів на кінцях. Якщо hasNum і останнє слово після пробілу — число, воно береться як
// числовий аргумент: "usb.fs.read /Папка 1/a.mp3 4096" → шлях "/Папка 1/a.mp3", num = 4096.
// Файл, чия назва закінчується на " <цифри>", так не адресується (див. «Відомі обмеження»).
// Повертає false при порожньому або задовгому шляху.
bool parsePath(const char* rest, bool hasNum, char* path, size_t pathSize, long* num) {
    if (rest == nullptr) {
        rest = "";
    }
    size_t len = strlen(rest);
    while (len > 0 && (rest[len - 1] == ' ' || rest[len - 1] == '\t')) {
        --len;
    }
    if (len == 0 || len >= pathSize) {
        path[0] = '\0';
        return false;
    }
    memcpy(path, rest, len);
    path[len] = '\0';
    if (num != nullptr) {
        *num = -1;
    }
    if (hasNum) {
        char* sp = strrchr(path, ' ');
        if (sp != nullptr && sp[1] != '\0') {
            bool digits = true;
            for (const char* p = sp + 1; *p != '\0'; ++p) {
                if (*p < '0' || *p > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                if (num != nullptr) {
                    *num = strtol(sp + 1, nullptr, 10);
                }
                *sp = '\0';
                size_t l = strlen(path);
                while (l > 0 && path[l - 1] == ' ') {
                    path[--l] = '\0';
                }
            }
        }
    }
    return path[0] != '\0';
}

// true, якщо том змонтовано. Інакше друкує причину; команди з VFS тоді не виконуються.
bool reportIfNotMounted(const char* cmd) {
    if (usb_msc_mount::isMounted()) {
        return true;
    }
    Serial.printf("%s fs.%s: volume not mounted (state=%s)\n", kTag, cmd,
                  usb_msc_mount::stateName(usb_msc_mount::state()));
    return false;
}

void printHelp() {
    Serial.printf("%s fs (read-only, / = volume root): usb.fs.ls [path] | usb.fs.exists <path> | "
                  "usb.fs.read <path> [bytes] | usb.fs.selftest <path> | usb.fs.cycle <path> <N> | "
                  "usb.fs.multi <path> <N> | usb.fs.help\n",
                  kTag);
}

void cmdFsLs(const char* rest) {
    char path[usb_host_cfg::kPathMax];
    if (!parsePath(rest, false, path, sizeof(path), nullptr)) {
        strcpy(path, "/");
    }
    if (!reportIfNotMounted("ls")) {
        return;
    }
    const uint32_t t0 = millis();
    fs::File dir = volume().open(path);
    if (!dir) {
        Serial.printf("%s fs.ls %s: open failed (missing or volume busy)\n", kTag, path);
        return;
    }
    if (!dir.isDirectory()) {
        Serial.printf("%s fs.ls %s: not a directory\n", kTag, path);
        dir.close();
        return;
    }
    int count = 0;
    bool truncated = false;
    for (;;) {
        fs::File e = dir.openNextFile();
        if (!e) {
            break;
        }
        if (count >= usb_host_cfg::kLsMaxLines) {
            truncated = true;
            e.close();
            break;
        }
        const bool isDir = e.isDirectory();
        Serial.printf("%s   %c %10lu  %s\n", kTag, isDir ? 'd' : '-',
                      (unsigned long)(isDir ? 0 : e.size()), e.name() != nullptr ? e.name() : "?");
        e.close();
        ++count;
    }
    dir.close();
    Serial.printf("%s fs.ls %s: listed %d%s, %lu ms\n", kTag, path, count,
                  truncated ? " (limit reached)" : "", (unsigned long)(millis() - t0));
}

void cmdFsExists(const char* rest) {
    char path[usb_host_cfg::kPathMax];
    if (!parsePath(rest, false, path, sizeof(path), nullptr)) {
        Serial.printf("%s usage: usb.fs.exists <path> (path empty or longer than %lu bytes)\n", kTag,
                      (unsigned long)usb_host_cfg::kPathMax);
        return;
    }
    reportIfNotMounted("exists");  // exists() все одно відповість false, без VFS
    const bool ex = volume().exists(path);
    Serial.printf("%s fs.exists %s: %s (path %lu bytes)\n", kTag, path, ex ? "yes" : "no",
                  (unsigned long)strlen(path));
}

void cmdFsRead(const char* rest) {
    char path[usb_host_cfg::kPathMax];
    long limit = -1;
    if (!parsePath(rest, true, path, sizeof(path), &limit)) {
        Serial.printf("%s usage: usb.fs.read <path> [bytes]\n", kTag);
        return;
    }
    if (!reportIfNotMounted("read")) {
        return;
    }
    fs::File f = volume().open(path);
    if (!f) {
        Serial.printf("%s fs.read %s: open failed\n", kTag, path);
        return;
    }
    const size_t total = f.size();
    size_t want = total;
    if (limit >= 0 && (size_t)limit < total) {
        want = (size_t)limit;
    }
    uint8_t* buf = (uint8_t*)malloc(usb_host_cfg::kFsChunk);
    if (buf == nullptr) {
        f.close();
        Serial.printf("%s fs.read %s: malloc failed\n", kTag, path);
        return;
    }
    size_t done = 0;
    uint32_t blocks = 0;
    uint32_t slow = 0;
    uint32_t maxUs = 0;
    bool err = false;
    const uint32_t startUs = micros();
    while (done < want) {
        const size_t n = (want - done < usb_host_cfg::kFsChunk) ? (want - done) : usb_host_cfg::kFsChunk;
        const uint32_t tb = micros();
        const size_t got = f.read(buf, n);
        const uint32_t dt = micros() - tb;
        if (got == 0 || got == (size_t)-1) {
            err = true;
            break;
        }
        done += got;
        ++blocks;
        if (dt > maxUs) {
            maxUs = dt;
        }
        if (dt >= 20000) {
            ++slow;
        }
        if (got < n) {
            break;  // короткий блок: кінець файлу
        }
    }
    const uint32_t elapsedUs = micros() - startUs;
    free(buf);
    f.close();
    const uint64_t kbps = elapsedUs > 0 ? ((uint64_t)done * 8000ULL) / elapsedUs : 0;
    Serial.printf("%s fs.read %s: size %lu B, read %lu B in %lu ms, %lu kbit/s\n", kTag, path,
                  (unsigned long)total, (unsigned long)done, (unsigned long)(elapsedUs / 1000),
                  (unsigned long)kbps);
    Serial.printf("%s fs.read: max read %lu.%03lu ms, blocks %lu (>=20 ms: %lu)%s\n", kTag,
                  (unsigned long)(maxUs / 1000), (unsigned long)(maxUs % 1000), (unsigned long)blocks,
                  (unsigned long)slow,
                  err ? " ERROR: read failed" : (done < want ? " (short read)" : ""));
}

void cmdFsSelftest(const char* rest) {
    char path[usb_host_cfg::kPathMax];
    if (!parsePath(rest, false, path, sizeof(path), nullptr)) {
        Serial.printf("%s usage: usb.fs.selftest <path>\n", kTag);
        return;
    }
    if (!reportIfNotMounted("selftest")) {
        Serial.printf("%s fs.selftest %s: FAIL (not mounted)\n", kTag, path);
        return;
    }
    fs::File f = volume().open(path);
    if (!f) {
        Serial.printf("%s fs.selftest %s: FAIL (open failed)\n", kTag, path);
        return;
    }
    const size_t total = f.size();
    const size_t blockSz = usb_host_cfg::kFsSelftestBlock;
    const size_t blocks = (total + blockSz - 1) / blockSz;
    const size_t stored = blocks < usb_host_cfg::kFsSelftestMaxBlocks ? blocks : usb_host_cfg::kFsSelftestMaxBlocks;
    uint32_t* crcs = (uint32_t*)malloc(sizeof(uint32_t) * (stored > 0 ? stored : 1));
    uint8_t* buf = (uint8_t*)malloc(usb_host_cfg::kFsChunk);
    if (crcs == nullptr || buf == nullptr) {
        free(crcs);
        free(buf);
        f.close();
        Serial.printf("%s fs.selftest %s: FAIL (malloc)\n", kTag, path);
        return;
    }

    bool ok = true;
    size_t pass1Blocks = 0;
    size_t off = 0;
    const uint32_t t0 = millis();
    // Прохід 1: послідовне читання всього файлу; CRC32 кожного блоку (перші stored блоків зберігаються).
    for (size_t b = 0; b < blocks && ok; ++b) {
        const size_t blockStart = b * blockSz;
        const size_t blockEnd = (blockStart + blockSz < total) ? blockStart + blockSz : total;
        uint32_t c = 0;
        while (off < blockEnd) {
            const size_t n = (blockEnd - off < usb_host_cfg::kFsChunk) ? (blockEnd - off) : usb_host_cfg::kFsChunk;
            const size_t got = f.read(buf, n);
            if (got == 0 || got == (size_t)-1) {
                ok = false;
                break;
            }
            c = crc32Update(c, buf, got);
            off += got;
        }
        if (!ok) {
            break;
        }
        if (b < stored) {
            crcs[b] = c;
        }
        ++pass1Blocks;
    }
    const uint32_t pass1Ms = millis() - t0;

    // Прохід 2: випадкові seek з перевіркою CRC (фіксоване зерно — прогін відтворюваний).
    uint32_t checks = 0;
    uint32_t mismatches = 0;
    if (ok && stored > 0) {
        uint32_t rng = 0x5EED1234u;
        for (uint32_t i = 0; i < usb_host_cfg::kFsSeekChecks && ok; ++i) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            const size_t b = rng % stored;
            const size_t start = b * blockSz;
            const size_t blockEnd = (start + blockSz < total) ? start + blockSz : total;
            if (!f.seek((uint32_t)start) || f.position() != start) {
                ok = false;
                break;
            }
            uint32_t c = 0;
            size_t pos = start;
            while (pos < blockEnd) {
                const size_t n = (blockEnd - pos < usb_host_cfg::kFsChunk) ? (blockEnd - pos) : usb_host_cfg::kFsChunk;
                const size_t got = f.read(buf, n);
                if (got == 0 || got == (size_t)-1) {
                    ok = false;
                    break;
                }
                c = crc32Update(c, buf, got);
                pos += got;
            }
            if (!ok) {
                break;
            }
            ++checks;
            if (c != crcs[b]) {
                ++mismatches;
                ok = false;
                Serial.printf("%s fs.selftest: CRC mismatch at block %lu\n", kTag, (unsigned long)b);
            }
        }
    }

    free(buf);
    free(crcs);
    f.close();
    const bool pass = ok && pass1Blocks == blocks && (stored == 0 || checks == usb_host_cfg::kFsSeekChecks);
    const char* why = pass ? "" : (mismatches > 0 ? " (CRC mismatch)" : " (read error)");
    Serial.printf("%s fs.selftest %s: %s%s\n", kTag, path, pass ? "PASS" : "FAIL", why);
    Serial.printf("%s fs.selftest: size %lu B, pass1 blocks %lu/%lu (64 KB, crc kept for %lu), pass1 %lu ms, "
                  "seek checks %lu/%lu, mismatches %lu\n",
                  kTag, (unsigned long)total, (unsigned long)pass1Blocks, (unsigned long)blocks,
                  (unsigned long)stored, (unsigned long)pass1Ms, (unsigned long)checks,
                  (unsigned long)usb_host_cfg::kFsSeekChecks, (unsigned long)mismatches);
    if (blocks > stored) {
        Serial.printf("%s fs.selftest: coverage partial, seek checks use the first %lu blocks only\n", kTag,
                      (unsigned long)stored);
    }
}

void cmdFsCycle(const char* rest) {
    char path[usb_host_cfg::kPathMax];
    long n = -1;
    if (!parsePath(rest, true, path, sizeof(path), &n) || n < 1 ||
        n > (long)usb_host_cfg::kCycleMaxCount) {
        Serial.printf("%s usage: usb.fs.cycle <path> <N> (1..%lu)\n", kTag,
                      (unsigned long)usb_host_cfg::kCycleMaxCount);
        return;
    }
    if (!reportIfNotMounted("cycle")) {
        return;
    }
    uint8_t* buf = (uint8_t*)malloc(usb_host_cfg::kFsChunk);
    if (buf == nullptr) {
        Serial.printf("%s fs.cycle %s: malloc failed\n", kTag, path);
        return;
    }
    const uint32_t free0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    long okCount = 0;
    long failCount = 0;
    for (long i = 0; i < n; ++i) {
        fs::File f = volume().open(path);
        if (!f) {
            ++failCount;
            continue;
        }
        const size_t got = f.read(buf, usb_host_cfg::kFsChunk);
        if (got == 0 || got == (size_t)-1) {
            ++failCount;
        } else {
            ++okCount;
        }
        f.close();
    }
    const uint32_t free1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest1 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    free(buf);
    Serial.printf("%s fs.cycle %s x%ld: ok %ld, fail %ld\n", kTag, path, n, okCount, failCount);
    Serial.printf("%s fs.cycle heap: free %lu -> %lu (diff %ld), largest %lu -> %lu (diff %ld)\n", kTag,
                  (unsigned long)free0, (unsigned long)free1, (long)free1 - (long)free0,
                  (unsigned long)largest0, (unsigned long)largest1, (long)largest1 - (long)largest0);
}

void cmdFsMulti(const char* rest) {
    char path[usb_host_cfg::kPathMax];
    long n = -1;
    if (!parsePath(rest, true, path, sizeof(path), &n) || n < 1 ||
        n > (long)usb_host_cfg::kFsMultiMax) {
        Serial.printf("%s usage: usb.fs.multi <path> <N> (1..%lu)\n", kTag,
                      (unsigned long)usb_host_cfg::kFsMultiMax);
        return;
    }
    if (!reportIfNotMounted("multi")) {
        return;
    }
    fs::File handles[usb_host_cfg::kFsMultiMax];
    long opened = 0;
    for (long i = 0; i < n; ++i) {
        handles[i] = volume().open(path);
        if (!handles[i]) {
            break;
        }
        ++opened;
    }
    if (opened < n) {
        Serial.printf("%s fs.multi %s: opened %ld of %ld at once, first refused open #%ld\n", kTag, path,
                      opened, n, opened + 1);
    } else {
        Serial.printf("%s fs.multi %s: opened %ld of %ld at once\n", kTag, path, opened, n);
    }
    for (long i = 0; i < opened; ++i) {
        handles[i].close();
    }
}

}  // namespace

void handleConsoleLine(const char* line) {
    if (line == nullptr) {
        return;
    }
    const char* rest = nullptr;
    if (matchWord(line, "usb.fs.ls", &rest)) {
        cmdFsLs(rest);
    } else if (matchWord(line, "usb.fs.exists", &rest)) {
        cmdFsExists(rest);
    } else if (matchWord(line, "usb.fs.read", &rest)) {
        cmdFsRead(rest);
    } else if (matchWord(line, "usb.fs.selftest", &rest)) {
        cmdFsSelftest(rest);
    } else if (matchWord(line, "usb.fs.cycle", &rest)) {
        cmdFsCycle(rest);
    } else if (matchWord(line, "usb.fs.multi", &rest)) {
        cmdFsMulti(rest);
    } else if (matchWord(line, "usb.fs.help", &rest)) {
        printHelp();
    } else {
        Serial.printf("%s fs: unknown command (usb.fs.help)\n", kTag);
    }
}

#endif  // USB_HOST_DIAG

}  // namespace usb_fs

#endif  // FEATURE_USB_HOST
