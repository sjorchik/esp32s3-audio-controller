#pragma once

// Список збережених Wi-Fi мереж (Prompt 29): до wifi_cfg::kMaxSavedNetworks записів
// «SSID + пароль»; порядок у списку = пріоритет (0 — найвищий).
//
// Зберігання: NVS namespace wifi_cfg::kNetNvsNamespace ("wifinets"), ключ "list" — ОДИН блоб
// із версією формату (запис атомарний: або старий список, або новий). Окремо від namespace
// audioctl (Settings) і від облікових даних, які сама бібліотека Arduino-ESP32 тримає в
// NVS розділі esp_wifi (WiFi.persistent(true), «остання підключена мережа»).
//
// Потоки: усі методи потокобезпечні (внутрішній мʼютекс, тайм-аут wifi_cfg::kLockTimeoutMs;
// при тайм-ауті — WifiNetResult::Busy, нічого не змінено). Зміни списку лише ПОЗНАЧАЮТЬ його
// «брудним»; у NVS список пише persistIfDirty() — його викликає задача WifiManager (запис у
// HTTP-обробниках, що працюють у задачі AsyncTCP, не виконується). clear() записує одразу.
//
// Паролі: лише в NVS і в RAM цього модуля; жоден метод, крім snapshot(), їх не віддає, а
// snapshot() призначений ТІЛЬКИ для net/wifi_manager.cpp. У лог паролі не потрапляють.

#include <stddef.h>
#include <stdint.h>

#include "config/wifi_config.h"

enum class WifiNetResult : uint8_t {
    Ok,
    Busy,              // мʼютекс зайнятий довше тайм-ауту (нічого не змінено)
    InvalidSsid,       // порожній, > 32 байтів або керівні символи
    InvalidPassword,   // не порожній і не 8..63 байти, або керівні символи
    ListFull,          // немає місця для нової мережі
    IndexOutOfRange,   // індекс мережі поза списком
    PositionOutOfRange // позиція вставки/переміщення поза межами
};

// Запис для зовнішнього світу (без пароля).
struct WifiNetInfo {
    char ssid[wifi_cfg::kSsidMax + 1];
    bool secure;  // true: пароль не порожній
};

// Повний запис (з паролем). Лише для net/wifi_manager.cpp.
struct WifiSavedNet {
    char ssid[wifi_cfg::kSsidMax + 1];
    char password[wifi_cfg::kPassMax + 1];
};

class WifiNetworks {
public:
    // Створює мʼютекси й читає список із NVS. Повторний виклик безпечний (повертає true).
    // false — не вдалося створити мʼютекс (методи тоді відповідають Busy).
    static bool begin();

    // true: у NVS знайдено коректний список (навіть порожній) або він записаний у цій сесії.
    // false: списку ще не було — WifiManager тоді переносить стару одиничну мережу з esp_wifi.
    static bool hasStoredList();

    // Перевірка введення без зміни списку (спільна для API й порталу).
    static WifiNetResult validate(const char* ssid, const char* password);

    // Додає або оновлює мережу.
    //  - SSID уже є: оновлення. password == nullptr -> пароль без змін; "" -> відкрита мережа.
    //    position >= 0 -> ще й перемістити на цю позицію (0..count-1); -1 -> лишити на місці.
    //  - SSID немає: нова. password == nullptr -> відкрита. position -1 -> в кінець, інакше
    //    вставити на позицію 0..count.
    //  - Список заповнений: ListFull; evictLastIfFull = true -> спершу видалити ОСТАННЮ
    //    (найнижчий пріоритет) — так робить портал AP, щоб користувач не застряг.
    // *indexOut — підсумкова позиція; *updatedOut — true для оновлення наявного запису.
    static WifiNetResult add(const char* ssid, const char* password, int position,
                             bool evictLastIfFull, uint8_t* indexOut = nullptr,
                             bool* updatedOut = nullptr);

    static WifiNetResult remove(uint8_t index);
    static WifiNetResult move(uint8_t from, uint8_t to);

    // Стирає ВСІ мережі й одразу записує порожній список (блокує до завершення запису).
    // Порожній блоб, а не видалення ключа: інакше наступний старт знову імпортував би стару
    // мережу з esp_wifi.
    static WifiNetResult clear();

    // Кількість записів (0 при Busy).
    static uint8_t count();

    // Копія списку без паролів. Повертає кількість (0..cap) або -1 при Busy.
    static int8_t list(WifiNetInfo* out, uint8_t cap);

    // Індекс за SSID (точний збіг) або -1 (також при Busy).
    static int8_t find(const char* ssid);

    // Копія списку З ПАРОЛЯМИ — лише для wifi_manager.cpp; викликач обовʼязково затирає її після
    // використання. Повертає кількість (0..cap) або -1 при Busy.
    static int8_t snapshot(WifiSavedNet* out, uint8_t cap);

    // Є незбережені зміни.
    static bool isDirty();

    // Пише список у NVS, якщо він «брудний». true — записано або писати не було чого;
    // false — не вдалося (список лишається «брудним», викликач повторить пізніше).
    static bool persistIfDirty();
};
