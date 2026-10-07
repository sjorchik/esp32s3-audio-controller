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

#include <stddef.h>
#include <stdint.h>

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
};
