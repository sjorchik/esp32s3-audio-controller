#pragma once

// HTTP-сервер (ESPAsyncWebServer): /api/status, GET/POST /api/settings.
// [Prompt 14] ДОДАНО маршрути /api/stations* (лише .cpp; цей інтерфейс не змінено).
// [Prompt 15] ДОДАНО маршрути /api/ir* (навчання пульта, мапа кодів; лише .cpp,
// інтерфейс не змінено). Навчання керується через AppController, не напряму.
//
// [Prompt 13] Заглушку замінено. ЗМІНЕНО: begin() тепер приймає вказівник на
// аудіопроцесор (так само, як AudioPlayer::begin / AppController::begin);
// poll() прибрано — AsyncWebServer обслуговується власною задачею async_tcp,
// опитувати ззовні нічого не треба.
//
// Сервер піднімається НЕ в begin(), а в одноразовій задачі-стартері, коли
// WifiManager повідомить Connected (STA). У режимі AP порт 80 займає captive
// portal з WifiManager, тому WebServerManager там мовчить.
//
// Виняток з «AppController — єдиний власник заліза» (узгоджений у Prompt 13):
// обробники напряму викликають DisplayManager::setBrightness/setFlipped та
// AudioProcessor::setLoudness. Тембр/баланс йдуть через AppController::setTone():
// AppController тримає власні копії цих значень і публікує їх після кожної події,
// тож пряма зміна чипа була б затерта (виявлено при читанні app_controller.cpp).
// setInput/мʼют/станції не чіпаються взагалі.

class AudioProcessor;

class WebServerManager {
public:
    // Запам'ятовує вказівник на процесор (nullptr дозволений: аудіополя POST
    // /api/settings тоді відповідають 503) і створює задачу-стартер.
    // Викликати ПІСЛЯ AppController::begin() (потрібні AppState і Settings) і
    // після WifiManager::begin(). Повторний виклик безпечний (повертає true).
    // false — не вдалося створити задачу.
    static bool begin(AudioProcessor* processorOrNull);
};
