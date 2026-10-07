// net/wifi_networks.cpp (Prompt 29): список збережених Wi-Fi мереж у NVS (див. wifi_networks.h).
//
// Блоб NVS: {version, count, reserved, nets[kMaxSavedNetworks]} фіксованого розміру. Читання
// відкидає блоб з іншою версією чи розміром (тоді hasStoredList() == false і WifiManager
// спробує перенести стару одиничну мережу з esp_wifi).
//
// Два мʼютекси: s_lock (дані в RAM, короткий) і s_nvsLock (серіалізує ЗАПИСИ в NVS). Запис
// завжди знімає копію ПОТОЧНОГО списку під s_lock, тож два писарі (задача wifi й одноразова
// задача перезапуску) не можуть перетерти новіший стан старішим.

#include "net/wifi_networks.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "config/wifi_config.h"

#if WIFI_MANAGER_DEBUG
#define NETS_LOG(...) Serial.printf("[WIFI] " __VA_ARGS__)
#else
#define NETS_LOG(...) \
    do {              \
    } while (0)
#endif

namespace {

constexpr uint8_t kMax = wifi_cfg::kMaxSavedNetworks;

struct StoredNets {
    uint8_t version;
    uint8_t count;
    uint16_t reserved;
    WifiSavedNet nets[kMax];
};

SemaphoreHandle_t s_lock = nullptr;     // дані в RAM
SemaphoreHandle_t s_nvsLock = nullptr;  // запис у NVS
bool s_beginDone = false;

WifiSavedNet s_nets[kMax];
uint8_t s_count = 0;
volatile bool s_dirty = false;
uint32_t s_rev = 0;           // збільшується при кожній зміні (під s_lock)
bool s_stored = false;        // є коректний список у NVS (або записаний у цій сесії)

// Затирання, яке компілятор не прибере.
void secureZero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n-- > 0) *v++ = 0;
}

class DataLock {
public:
    DataLock()
        : m_ok(s_lock != nullptr &&
               xSemaphoreTake(s_lock, pdMS_TO_TICKS(wifi_cfg::kLockTimeoutMs)) == pdTRUE) {}
    ~DataLock() {
        if (m_ok) xSemaphoreGive(s_lock);
    }
    DataLock(const DataLock&) = delete;
    DataLock& operator=(const DataLock&) = delete;
    bool ok() const { return m_ok; }

private:
    bool m_ok;
};

bool hasControl(const char* s) {
    for (; *s != '\0'; ++s) {
        const uint8_t c = static_cast<uint8_t>(*s);
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

int findLocked(const char* ssid) {
    for (uint8_t i = 0; i < s_count; ++i) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

void markDirtyLocked() {
    ++s_rev;
    s_dirty = true;
}

// Вставка/видалення зі зсувом (під s_lock).
void removeAtLocked(uint8_t index) {
    for (uint8_t i = index; i + 1 < s_count; ++i) s_nets[i] = s_nets[i + 1];
    --s_count;
    secureZero(&s_nets[s_count], sizeof(WifiSavedNet));
}

void insertAtLocked(uint8_t pos, const WifiSavedNet& net) {
    for (uint8_t i = s_count; i > pos; --i) s_nets[i] = s_nets[i - 1];
    s_nets[pos] = net;
    ++s_count;
}

// Записує поточний список у NVS. timeoutMs — скільки чекати на s_nvsLock.
bool persistImpl(uint32_t timeoutMs) {
    if (s_nvsLock == nullptr || xSemaphoreTake(s_nvsLock, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return false;
    }
    StoredNets blob;
    memset(&blob, 0, sizeof(blob));
    uint32_t rev = 0;
    bool needed = false;
    {
        DataLock l;
        if (l.ok()) {
            if (s_dirty) {
                blob.version = wifi_cfg::kNetFormatVersion;
                blob.count = s_count;
                for (uint8_t i = 0; i < s_count; ++i) blob.nets[i] = s_nets[i];
                rev = s_rev;
                needed = true;
            }
        } else {
            xSemaphoreGive(s_nvsLock);
            return false;
        }
    }
    bool ok = true;
    if (needed) {
        Preferences p;
        ok = p.begin(wifi_cfg::kNetNvsNamespace, false);
        if (ok) {
            ok = (p.putBytes(wifi_cfg::kNetNvsKey, &blob, sizeof(blob)) == sizeof(blob));
            p.end();
        }
        if (ok) {
            DataLock l;
            if (l.ok()) {
                if (s_rev == rev) s_dirty = false;  // за час запису список міг змінитись ще раз
                s_stored = true;
            } else {
                s_stored = true;  // запис у NVS є; прапорець «брудний» залишиться — безпечно
            }
            NETS_LOG("network list saved (%u)\n", static_cast<unsigned>(blob.count));
        } else {
            NETS_LOG("network list save FAILED\n");
        }
    }
    secureZero(&blob, sizeof(blob));
    xSemaphoreGive(s_nvsLock);
    return ok;
}

}  // namespace

// ---------------------------------------------------------------------------
bool WifiNetworks::begin() {
    if (s_beginDone) return true;
    s_lock = xSemaphoreCreateMutex();
    s_nvsLock = xSemaphoreCreateMutex();
    if (s_lock == nullptr || s_nvsLock == nullptr) {
        Serial.println("[WIFI] network list: mutex create failed");
        return false;
    }

    StoredNets blob;
    memset(&blob, 0, sizeof(blob));
    Preferences p;
    // Режим читання-запису: у режимі «лише читання» відсутній namespace дає помилку в логах.
    if (p.begin(wifi_cfg::kNetNvsNamespace, false)) {
        const size_t len = p.getBytesLength(wifi_cfg::kNetNvsKey);
        if (len == sizeof(blob) &&
            p.getBytes(wifi_cfg::kNetNvsKey, &blob, sizeof(blob)) == sizeof(blob) &&
            blob.version == wifi_cfg::kNetFormatVersion && blob.count <= kMax) {
            s_count = 0;
            for (uint8_t i = 0; i < blob.count; ++i) {
                // Захист від пошкоджених рядків: примусово завершуємо '\0'; невалідне — пропуск.
                blob.nets[i].ssid[wifi_cfg::kSsidMax] = '\0';
                blob.nets[i].password[wifi_cfg::kPassMax] = '\0';
                if (WifiNetworks::validate(blob.nets[i].ssid, blob.nets[i].password) !=
                        WifiNetResult::Ok ||
                    findLocked(blob.nets[i].ssid) >= 0) {
                    NETS_LOG("list entry %u skipped (invalid or duplicate)\n",
                             static_cast<unsigned>(i));
                    continue;
                }
                s_nets[s_count++] = blob.nets[i];
            }
            s_stored = true;
            if (s_count != blob.count) s_dirty = true;  // очистити пошкоджене при наступному записі
        } else if (len != 0) {
            NETS_LOG("stored list ignored (size %u / version mismatch)\n",
                     static_cast<unsigned>(len));
        }
        p.end();
    } else {
        NETS_LOG("network list: NVS open failed\n");
    }
    secureZero(&blob, sizeof(blob));
    s_beginDone = true;
    NETS_LOG("network list: %u saved, stored=%s\n", static_cast<unsigned>(s_count),
             s_stored ? "yes" : "no");
    return true;
}

bool WifiNetworks::hasStoredList() {
    DataLock l;
    return l.ok() ? s_stored : true;  // при Busy не запускаємо перенесення зі старої мережі
}

WifiNetResult WifiNetworks::validate(const char* ssid, const char* password) {
    if (ssid == nullptr) return WifiNetResult::InvalidSsid;
    const size_t sl = strlen(ssid);
    if (sl < 1 || sl > wifi_cfg::kSsidMax || hasControl(ssid)) return WifiNetResult::InvalidSsid;
    if (password != nullptr) {
        const size_t pl = strlen(password);
        if (pl > wifi_cfg::kPassMax || (pl > 0 && pl < 8) || hasControl(password)) {
            return WifiNetResult::InvalidPassword;
        }
    }
    return WifiNetResult::Ok;
}

WifiNetResult WifiNetworks::add(const char* ssid, const char* password, int position,
                                bool evictLastIfFull, uint8_t* indexOut, bool* updatedOut) {
    const WifiNetResult v = validate(ssid, password);
    if (v != WifiNetResult::Ok) return v;

    DataLock l;
    if (!l.ok()) return WifiNetResult::Busy;

    const int existing = findLocked(ssid);
    if (existing >= 0) {
        // Оновлення: спершу перевірки, потім зміни (при помилці нічого не змінено).
        if (position >= static_cast<int>(s_count)) return WifiNetResult::PositionOutOfRange;
        bool changed = false;
        if (password != nullptr && strcmp(s_nets[existing].password, password) != 0) {
            strlcpy(s_nets[existing].password, password, sizeof(s_nets[existing].password));
            changed = true;
        }
        int at = existing;
        if (position >= 0 && position != existing) {
            WifiSavedNet tmp = s_nets[existing];
            removeAtLocked(static_cast<uint8_t>(existing));
            insertAtLocked(static_cast<uint8_t>(position), tmp);
            secureZero(&tmp, sizeof(tmp));
            at = position;
            changed = true;
        }
        if (changed) markDirtyLocked();
        if (indexOut != nullptr) *indexOut = static_cast<uint8_t>(at);
        if (updatedOut != nullptr) *updatedOut = true;
        return WifiNetResult::Ok;
    }

    // Нова мережа.
    if (position < -1) return WifiNetResult::PositionOutOfRange;
    if (s_count >= kMax) {
        if (!evictLastIfFull) return WifiNetResult::ListFull;
        NETS_LOG("list full: dropping lowest-priority network \"%s\"\n", s_nets[s_count - 1].ssid);
        removeAtLocked(static_cast<uint8_t>(s_count - 1));
    }
    if (position > static_cast<int>(s_count)) return WifiNetResult::PositionOutOfRange;
    const uint8_t at = (position < 0) ? s_count : static_cast<uint8_t>(position);

    WifiSavedNet net;
    memset(&net, 0, sizeof(net));
    strlcpy(net.ssid, ssid, sizeof(net.ssid));
    if (password != nullptr) strlcpy(net.password, password, sizeof(net.password));
    insertAtLocked(at, net);
    secureZero(&net, sizeof(net));
    markDirtyLocked();
    if (indexOut != nullptr) *indexOut = at;
    if (updatedOut != nullptr) *updatedOut = false;
    return WifiNetResult::Ok;
}

WifiNetResult WifiNetworks::remove(uint8_t index) {
    DataLock l;
    if (!l.ok()) return WifiNetResult::Busy;
    if (index >= s_count) return WifiNetResult::IndexOutOfRange;
    removeAtLocked(index);
    markDirtyLocked();
    return WifiNetResult::Ok;
}

WifiNetResult WifiNetworks::move(uint8_t from, uint8_t to) {
    DataLock l;
    if (!l.ok()) return WifiNetResult::Busy;
    if (from >= s_count || to >= s_count) return WifiNetResult::IndexOutOfRange;
    if (from == to) return WifiNetResult::Ok;
    WifiSavedNet tmp = s_nets[from];
    removeAtLocked(from);
    insertAtLocked(to, tmp);
    secureZero(&tmp, sizeof(tmp));
    markDirtyLocked();
    return WifiNetResult::Ok;
}

WifiNetResult WifiNetworks::clear() {
    {
        DataLock l;
        if (!l.ok()) return WifiNetResult::Busy;
        secureZero(s_nets, sizeof(s_nets));
        s_count = 0;
        markDirtyLocked();
    }
    return persistImpl(wifi_cfg::kNvsClearLockTimeoutMs) ? WifiNetResult::Ok : WifiNetResult::Busy;
}

uint8_t WifiNetworks::count() {
    DataLock l;
    return l.ok() ? s_count : 0;
}

int8_t WifiNetworks::list(WifiNetInfo* out, uint8_t cap) {
    DataLock l;
    if (!l.ok()) return -1;
    const uint8_t n = (s_count < cap) ? s_count : cap;
    for (uint8_t i = 0; i < n; ++i) {
        strlcpy(out[i].ssid, s_nets[i].ssid, sizeof(out[i].ssid));
        out[i].secure = (s_nets[i].password[0] != '\0');
    }
    return static_cast<int8_t>(n);
}

int8_t WifiNetworks::find(const char* ssid) {
    DataLock l;
    if (!l.ok() || ssid == nullptr) return -1;
    return static_cast<int8_t>(findLocked(ssid));
}

int8_t WifiNetworks::snapshot(WifiSavedNet* out, uint8_t cap) {
    DataLock l;
    if (!l.ok()) return -1;
    const uint8_t n = (s_count < cap) ? s_count : cap;
    for (uint8_t i = 0; i < n; ++i) out[i] = s_nets[i];
    return static_cast<int8_t>(n);
}

bool WifiNetworks::isDirty() { return s_dirty; }

bool WifiNetworks::persistIfDirty() {
    if (!s_dirty) return true;
    return persistImpl(wifi_cfg::kNvsLockTimeoutMs);
}
