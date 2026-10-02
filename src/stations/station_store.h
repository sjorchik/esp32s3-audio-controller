#pragma once

// Сховище станцій у LittleFS, формат JSON.
// Імпорт: M3U / PLS / JSON.
// Експорт: JSON.
//
// [Prompt 11] Заглушку замінено реалізацією (station_store.cpp). Публічний API
// і struct Station НЕ змінено.
//
// StationStore — модуль ЧИСТИХ ДАНИХ (як UiFonts/UiIcons): не апаратний, не
// читає EventBus, не впливає на звуковий тракт. Тому його можуть читати
// напряму і AppController, і ui/screens (а згодом веб-сервер).
//
// Потокобезпечність: усі методи можна викликати з будь-якої задачі.
// get()/count() беруть короткий мьютекс; імпорт/експорт серіалізуються окремим
// мьютексом і тривають довго (файловий ввід/вивід) — не викликати з
// UI-задачі чи з обробника події.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Опис станції.
struct Station {
    uint16_t id;      // індекс у списку (заповнюється при читанні)
    char name[64];
    char url[192];
};

class StationStore {
public:
    // Ініціалізація сховища: завантажує /stations.json (station_store_cfg::kStoragePath).
    // Якщо файл відсутній, порожній, пошкоджений або іншої версії — одноразовий
    // seed вбудованого списку з негайним записом у файл. Повторні виклики
    // безпечні. true — є хоча б одна станція (навіть якщо запис у файл не вдався).
    // LittleFS має бути змонтовано раніше (main.cpp).
    static bool begin();

    // Кількість станцій (0 до успішного begin()).
    static size_t count();

    // Отримати станцію за індексом: копіює в out (out.id = index). false —
    // індекс за межами списку або модуль не готовий.
    static bool get(size_t index, Station& out);

    // Імпорт списків відтворення з файлу, що вже лежить у LittleFS. Повністю
    // заміняє список; при помилці (парсинг, переповнення, 0 валідних станцій,
    // збій запису) старий список лишається незмінним. Після успіху список
    // записано у сховище.
    static bool importM3u(const char* path);
    static bool importPls(const char* path);
    static bool importJson(const char* path);

    // Експорт: записує поточний список у path (може збігатися зі сховищем).
    static bool exportJson(const char* path);
};
