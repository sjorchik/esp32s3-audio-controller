#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "core/events.h"

// Результат декодування одного кадру RC5/RC5X.
struct IrRc5Frame {
    uint8_t addr;          // 0..31
    uint8_t cmd;           // 0..127 (для RC5X 7-й біт вже інвертовано з S2)
    bool toggle;
    bool isRC5X;           // true, якщо S2 == 0 (тоді cmd >= 64)
    uint32_t timestampMs;  // millis() на момент обробки кадру
};

// IR-приймач RC5/RC5X: RMT RX, мапа кодів у NVS, режим навчання.
// Усі методи, крім begin(), викликати після begin().
class IrRc5 {
public:
    enum class LearnStatus : uint8_t {
        Idle,      // навчання не активне
        Waiting,   // чекаємо перше натискання
        Confirm,   // перший код прийнято, чекаємо такого ж другого натискання
        Success,   // код привʼязано й збережено
        Timeout,   // вичерпано час очікування
        Conflict,  // код уже привʼязаний до іншої дії; чекаємо confirmOverwrite()
    };

    // Створює задачу приймання, налаштовує RMT RX, завантажує мапу з NVS.
    // Потребує готового EventBus. Повторний виклик безпечний (повертає true).
    static bool begin();

    // Скільки подій відкинуто через переповнену чергу EventBus.
    static uint32_t droppedEvents();

    // Скільки прийнятих кадрів відкинуто як невалідні (діагностика шуму).
    static uint32_t decodeErrors();

    // --- Мапа кодів (потокобезпечна) ---

    // Завантажує мапу з NVS. true — прочитано валідну мапу (можливо, порожню).
    // false — нічого не збережено або дані пошкоджені (мапа очищена).
    static bool load();

    // Зберігає мапу в NVS.
    static bool save();

    // Очищає мапу в памʼяті й у NVS.
    static void clearMap();

    // Видаляє всі коди дії й зберігає. true, якщо щось було видалено.
    static bool removeAction(Action action);

    // Дія за кодом (addr, cmd).
    static bool lookup(uint8_t addr, uint8_t cmd, Action& out);

    // Перший код, привʼязаний до дії.
    static bool codeFor(Action action, uint8_t& addr, uint8_t& cmd);

    // Кількість записів у мапі.
    static size_t mapSize();

    // --- Навчання ---

    // Починає навчання для дії. Звичайна генерація подій призупиняється.
    // false — дія некоректна або мапа заповнена (і в дії немає власного запису).
    static bool beginLearn(Action action);

    // Завершує навчання будь-якого стану (у т.ч. скидає Success/Timeout в Idle).
    static void cancelLearn();

    // Поточний стан. Success/Timeout лишаються, доки не буде beginLearn/cancelLearn.
    static LearnStatus status();

    // Підтвердити перепризначення в стані Conflict (старий привʼязаний до
    // цього коду запис іншої дії видаляється). true — статус став Success.
    static bool confirmOverwrite();

    // Для UI: дія, яку навчають.
    static Action learnTarget();

    // Для UI: кандидат (доступний у Confirm/Conflict).
    static bool learnCandidate(uint8_t& addr, uint8_t& cmd);

    // Для UI: з якою дією конфлікт (доступно лише в Conflict).
    static bool learnConflictWith(Action& other);

    // --- JSON (для майбутнього веб-інтерфейсу) ---

    // Записує мапу як масив: [{"action":"VOL_UP","addr":0,"cmd":16,"rc5x":false}, ...]
    // false — документ переповнено.
    static bool exportJson(JsonDocument& doc);

    // Замінює мапу вмістом масиву й зберігає в NVS. Атомарно: при будь-якій
    // помилці валідації мапа не змінюється. false також під час навчання
    // або якщо не вдалося зберегти в NVS (тоді мапа в RAM уже оновлена).
    static bool importJson(const JsonDocument& doc);
};
