#pragma once

// HTTP-сервер (ESPAsyncWebServer): /api/status, GET/POST /api/settings.
// [Prompt 14] ДОДАНО маршрути /api/stations* (лише .cpp; цей інтерфейс не змінено).
// [Prompt 15] ДОДАНО маршрути /api/ir* (навчання пульта, мапа кодів; лише .cpp,
// інтерфейс не змінено). Навчання керується через AppController, не напряму.
//
// [Prompt 48a] ЗМІНЕНО: аудіочіпи вирізано, begin() БЕЗ параметрів (було begin(AudioProcessor*)).
// [Prompt 13] Заглушку замінено. (Колишній опис: begin() приймав вказівник на аудіопроцесор.)
// poll() прибрано — AsyncWebServer обслуговується власною задачею async_tcp,
// опитувати ззовні нічого не треба.
//
// Сервер піднімається НЕ в begin(), а в одноразовій задачі-стартері, коли
// WifiManager повідомить Connected (STA). У режимі AP порт 80 займає captive
// portal з WifiManager, тому WebServerManager там мовчить.
//
// Виняток з «AppController — єдиний власник заліза» (узгоджений у Prompt 13):
// обробники напряму викликають DisplayManager::setBrightness/setFlipped.
// [P48a] Тембр / баланс / loudness більше не підтримуються (400 not_supported).
// setInput/мʼют/станції не чіпаються взагалі.

class WebServerManager {
public:
    // Створює задачу-стартер.
    // Викликати ПІСЛЯ AppController::begin() (потрібні AppState і Settings) і
    // після WifiManager::begin(). Повторний виклик безпечний (повертає true).
    // false — не вдалося створити задачу.
    static bool begin();
};
