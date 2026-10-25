#pragma once

// Wi-Fi менеджер (Prompt 12).
// Режим станції (STA) за збереженими обліковими даними; 3 невдалі спроби поспіль
// після старту (або відсутність збереженої мережі, або скидання утриманням OK) —
// режим AP "AudioCtrl-Setup" із captive portal (DNS + HTTP): скан мереж, вибір,
// пароль -> WiFi.begin() -> перезапуск у STA.
//
// Облікові дані зберігає сама бібліотека Arduino-ESP32 (NVS-розділ esp_wifi,
// WiFi.persistent(true)); у Settings вони не дублюються.
//
// Усі мережеві виклики виконує власна задача (ядро 0, найнижчий пріоритет).
// Гетери потокобезпечні й неблокуючі (короткий мʼютекс).
//
// [Prompt 12] Заглушку замінено. ЗМІНЕНО: begin() тепер приймає forceReset;
// poll() прибрано (задача внутрішня, нічого опитувати ззовні не треба).
// [Prompt 28] ДОДАНО: stop() і WifiState::Off — повне вимкнення Wi-Fi для офлайн-режиму
// (AP, DNS, портал, WiFi.mode(WIFI_OFF)). Облікові дані й провізіонінг не змінено.
// [Prompt 29] ДОДАНО: кілька збережених мереж (список — net/wifi_networks.h; тут лише вибір
// мережі серед них), скан у режимі STA, примусове підключення до збереженої мережі, канал.
// Існуючі методи й їхня семантика не змінені. Облікові дані тепер ТРИМАЄ wifi_networks
// (NVS namespace "wifinets"); стара одинична мережа з esp_wifi переноситься в список автоматично.
// Збереження в esp_wifi (WiFi.persistent(true)) лишилось як «остання підключена мережа».

#include <stddef.h>
#include <stdint.h>

// [Prompt 29] Стан скану (спільний для порталу AP, вибору мережі й GET /api/wifi/scan).
enum class WifiScanState : uint8_t {
    Idle,     // скану ще не було
    Running,  // триває
    Done,     // є результати (copyScan)
    Failed,   // не вдалося запустити або вийшов час
};

// [Prompt 29] Результат скану для API (без прихованих мереж; кілька точок з одним SSID зведено
// до найсильнішої).
struct WifiScanEntry {
    char ssid[33];  // 32 байти + '\0' (== wifi_cfg::kSsidMax + 1)
    int8_t rssi;
    uint8_t channel;
    bool secure;
};

// [Prompt 29] Результат запиту до задачі WifiManager.
enum class WifiRequest : uint8_t {
    Accepted,       // прийнято, виконає задача wifi
    LockBusy,       // внутрішній мʼютекс зайнятий -> HTTP 503
    NotReady,       // зараз не той стан (іде підключення, AP, офлайн)
    ScanBusy,       // скан уже триває
    Pending,        // попередній запит на підключення ще не оброблено
    NoSuchNetwork,  // індекс поза списком збережених
    AlreadyCurrent, // це вже поточна мережа (connect нічого не робить)
};

enum class WifiState : uint8_t {
    Connecting,         // STA: перша спроба, повтор після втрати звʼязку
    Connected,          // STA: є IP
    ApMode,             // AP піднято, клієнтів немає
    ApClientConnected,  // AP піднято, до нього підключений хоча б один пристрій
    Off,                // [Prompt 28] Wi-Fi вимкнено повністю (офлайн); вихід лише перезапуском
};

class WifiManager {
public:
    // Створює задачу й одразу повертається. forceReset = true: стерти збережену
    // мережу й піднімати AP. Повторний виклик безпечний (повертає true).
    static bool begin(bool forceReset);

    // [Prompt 28] ДОДАНО: просить задачу WifiManager повністю вимкнути Wi-Fi: зупинити
    // DNS і HTTP-портал, AP, STA та викликати WiFi.mode(WIFI_OFF). НЕ блокує: лише ставить
    // запит, а саме вимкнення робить внутрішня задача (єдиний власник WiFi.*) протягом
    // одного періоду опитування; після цього state() == Off. Збережена мережа в NVS не
    // стирається. Повернення в онлайн — лише перезапуск контролера. Безпечно викликати
    // повторно й з будь-якої задачі; до begin() запит нічого не робить.
    static void stop();

    static WifiState state();
    static bool isConnected();  // state() == Connected
    static bool isApMode();     // ApMode або ApClientConnected

    // Поточна мережа (STA) або назва AP; порожній рядок, якщо невідомо.
    // Вказівник на внутрішній буфер: може змінитись при переході стану.
    // Для узгодженої пари ssid+ip з іншої задачі користуйтеся copyInfo().
    static const char* ssid();
    // IP: отриманий (STA) чи адреса AP ("192.168.4.1"); порожній рядок, якщо немає.
    static const char* ipAddress();

    // Потокобезпечна копія ssid та ip (завжди завершена '\0'; хвіст буфера обнулено).
    static void copyInfo(char* ssidOut, size_t ssidCap, char* ipOut, size_t ipCap);

    // RSSI поточної STA-мережі, дБм (кеш, оновлюється раз на wifi_cfg::kRssiPeriodMs).
    // 0, якщо STA не підключена.
    static int32_t rssi();

    // [Prompt 29] ДОДАНО. Канал поточної STA-мережі (кеш, як rssi()); 0, якщо не підключена.
    static int32_t channel();

    // [Prompt 29] ДОДАНО. Просить задачу запустити скан (STA). НЕ блокує; результат — через
    // scanState()/copyScan(). Приймається лише коли пристрій підключений (Connected).
    // Інакше NotReady; скан уже триває — ScanBusy.
    static WifiRequest requestScan();

    // [Prompt 29] ДОДАНО. Стан останнього скану; *ageMsOut (якщо не nullptr) — скільки мс
    // тому він завершився (лише для Done).
    static WifiScanState scanState(uint32_t* ageMsOut = nullptr);

    // [Prompt 29] ДОДАНО. Копія результатів останнього успішного скану (за спаданням RSSI),
    // не більше cap записів; повертає кількість. 0 при зайнятому мʼютексі.
    static uint8_t copyScan(WifiScanEntry* out, uint8_t cap);

    // [Prompt 29] ДОДАНО. Просить задачу негайно перемкнутись на збережену мережу з позицією
    // index у списку wifi_networks. НЕ блокує; задача чекає wifi_cfg::kConnectReplyFlushMs
    // (щоб HTTP-відповідь дійшла), потім пробує саме цю мережу; невдача -> звичайний вибір
    // серед збережених (без AP). Приймається в станах Connected і повторного підключення.
    static WifiRequest requestConnect(uint8_t index);
};
