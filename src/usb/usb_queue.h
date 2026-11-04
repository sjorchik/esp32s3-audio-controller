#pragma once

// [USB10] Черга відтворення (крок g2a, лише бекенд): файли ПОТОЧНОЇ папки браузера (без рекурсії), на якій запущено
// відтворення; автоперехід на наступний трек за прапорцем AudioPlayer::takeFileEnded(); послідовний режим і перемішування;
// повтор усього; пропуск файлів, що не відкрились.
//
// Правила:
//  - Черга = знімок файлів папки на момент playIndex (cd після цього її не змінює). Порядок = порядок списку браузера.
//  - Послідовно: по порядку списку. Кінець черги при автопереході: стоп, якщо немає повтору (reason «end_of_queue»),
//    інакше з першого. Ручний next на останньому треку без повтору — відмова «end_of_queue», відтворення триває.
//    prev на першому: з повтором — останній, без — той самий трек з початку.
//  - Перемішування: кожен трек один раз за прохід у випадковому порядку (Fisher–Yates, esp_random); обраний трек — першим.
//    Повтор усього — новий прохід (перший трек не збігається з щойно зіграним). prev — за історією (kHistoryMax), next після
//    prev повертає на покинутий трек; порожня історія — трек з початку.
//  - setShuffle() під час відтворення поточний трек не переривають: порядок перебудовується, поточний стає першим у проході.
//  - Файл, що не відкрився / збій читання: пропуск; після kMaxConsecutiveSkips поспіль — стоп «too_many_skips».
//  - Вийняття флешки / вихід із режиму: черга зупиняється (reason «media_removed» / «mode_exit»), список браузера недійсний.
//  - setShuffle / setRepeatAll живуть лише в RAM (зберігаються між сеансами режиму до перезавантаження; NVS — крок g2b).
//
// Потоки: як usb_browser (задача usb_nav); усі функції неблокуючі й безпечні з будь-якої задачі.
// Заголовок не підключає заголовки IDF, AppController, AudioPlayer.

#include <stddef.h>
#include <stdint.h>

#include "config/features.h"

#if FEATURE_USB_MODE

namespace usb_queue {

enum class State : uint8_t {
    Idle,      // черга не веде відтворення (не запущена або зупинена; причина — Status::reason)
    Starting,  // файл запитано, чекаємо Playing
    Playing,   // файл грає (або дограє хвіст перед автопереходом)
};

struct Status {
    State state;
    bool shuffle;
    bool repeatAll;
    uint16_t count;      // файлів у черзі
    int32_t index;       // поточний трек (номер файлу в черзі, 0..count-1); -1 — немає
    int32_t position;    // позиція в порядку проходу (0..count-1); -1 — немає
    uint8_t skips;       // пропусків поспіль
    uint32_t started;    // скільки треків дійшло до Playing з початку сеансу черги
    uint32_t version;    // змінюється при кожній зміні стану
    const char* reason;  // причина останньої зупинки (статичний рядок) або nullptr
};

const char* stateName(State s);

// Знімок. false — мьютекс зайнятий довше usb_browser_cfg::kApiLockWaitMs.
bool status(Status* out);
// Копіює повний шлях поточного треку (від кореня флешки). false — треку нема / малий буфер / мьютекс зайнятий.
bool currentPath(char* out, size_t cap);

// Запити (неблокуючі), результат — usb_browser::status().lastReqId / lastOk / lastWhy.
// playIndex: browserIndex — номер ФАЙЛУ в списку браузера (не папки); expectVersion — версія списку (0 — не перевіряти).
bool requestPlayIndex(uint16_t browserIndex, uint32_t expectVersion = 0, uint32_t* reqId = nullptr, const char** why = nullptr);
bool requestNext(uint32_t* reqId = nullptr, const char** why = nullptr);
bool requestPrev(uint32_t* reqId = nullptr, const char** why = nullptr);
bool requestStop(uint32_t* reqId = nullptr, const char** why = nullptr);

// Налаштування (RAM). Застосовуються задачею usb_nav не пізніше ≈ kPollMs.
void setShuffle(bool on);
bool shuffle();
void setRepeatAll(bool on);
bool repeatAll();

#if USB_HOST_DIAG
// Консоль: usb.q play <індекс> | next | prev | stop | stat | shuffle on|off | repeat on|off. Блокує loopTask до завершення.
bool isConsoleCommand(const char* line);
void handleConsoleLine(const char* line);
#endif

}  // namespace usb_queue

#endif  // FEATURE_USB_MODE
