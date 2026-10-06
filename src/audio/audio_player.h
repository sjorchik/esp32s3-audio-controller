#pragma once

// Аудіоплеєр інтернет-радіо: обгортка над ESP32-audioI2S.
//
// - I2S ініціалізує ВИКЛЮЧНО бібліотека (BCLK/WS/DOUT з config/pins.h).
// - Гучність бібліотеки завжди максимальна; гучність керується AudioProcessor.
// - Софт-мʼют ЦАП PCM5102 (XSMT, pins::kXsmt) контролює цей модуль. Мʼют
//   атенюаторів AudioProcessor::setMute() цей модуль НЕ викликає — це
//   відповідальність AppController (розділ 11 MASTER SPEC).
// - Усі команди (playUrl/stop) асинхронні: вони лише ставлять запит, а виконує
//   його задача плеєра (ядро 1) — бібліотека не потокобезпечна.
// - Публічні гетери потокобезпечні (викликати можна з будь-якої задачі).
//
// [Prompt 25] ДОДАНО: setOutputTrimDb()/outputTrimDb() — рівень виходу декодера по станціях.
//
// [Prompt 5] Заглушку замінено. Позначки:
//   ЗМІНЕНО — сигнатура відрізняється від заглушки (розділ 12);
//   ДОДАНО  — нове.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

class AudioProcessor;

// ДОДАНО: стан підключення для UI.
//  Idle         — нічого не відтворюється (початковий стан і після stop()).
//  Connecting   — перша спроба підключення до станції (виклик блокує задачу
//                 плеєра, тому ззовні цей стан видно лише короткочасно).
//  Buffering    — зʼєднання є, наповнюється буфер (старт) або він просів
//                 (недовантаження під час відтворення).
//  Playing      — потік грає.
//  Error        — потік обірвався/не підключився; чекаємо часу наступної спроби.
//  Reconnecting — повторна спроба підключення після Error (виклик у процесі).
enum class PlayerState : uint8_t {
    Idle,
    Connecting,
    Playing,
    Buffering,
    Error,
    Reconnecting,
};

class AudioPlayer {
public:
    // ЗМІНЕНО: begin() приймає опціональний вказівник на аудіопроцесор.
    // Плеєр НЕ володіє ним і поки лише зберігає (для майбутньої інтеграції);
    // AudioProcessor::setMute() не викликається. nullptr дозволений.
    // Створює обʼєкт Audio, налаштовує I2S (через бібліотеку), встановлює XSMT у
    // мʼют і створює задачу плеєра (ядро 1). Повторний виклик безпечний.
    static bool begin(AudioProcessor* processorOrNull = nullptr);

    // Запит на відтворення URL (http/https-потік, а також m3u/pls-плейлист,
    // який бібліотека розбирає сама). true — запит прийнято (не означає, що
    // потік уже грає). Новий запит під час відтворення перемикає станцію.
    static bool playUrl(const char* url);

    // Запит на зупинку. true — запит прийнято. Скасовує перепідключення.
    static bool stop();

    // true у станах Playing і Buffering (потік активний; для AppState.streamPlaying).
    static bool isPlaying();

    // Тіло задачі плеєра. Не викликати вручну: його запускає begin().
    static void taskLoop();

    // --- ДОДАНО: стан, метадані, діагностика ---

    static PlayerState state();
    static const char* stateName(PlayerState s);

    // Копіює поточні назву станції та заголовок треку (УЖЕ UTF-8). Повертає
    // true, якщо хоча б один із рядків непорожній. Рядки завжди завершуються
    // '\0' (якщо cap > 0); обрізання — по межі UTF-8-символу.
    static bool currentMetadata(char* stationOut, size_t stationCap,
                                char* titleOut, size_t titleCap);

    // Скільки разів запущено повторне підключення (кожна спроба після Error).
    static uint32_t reconnectCount();

    // Скільки разів буфер просідав під час стабільного відтворення
    // (перехід Playing -> Buffering). Рахується нами за заповненням буфера, а не
    // за callback-ом бібліотеки.
    static uint32_t bufferUnderrunCount();

    // Останній HTTP-код, побачений у повідомленнях бібліотеки; 0 — невідомий.
    // Залежить від того, чи друкує ваша версія бібліотеки рядок "HTTP/1.x NNN".
    static int lastHttpCode();

    // Перекодування ICY-рядка (cp1251 / UTF-8 / Latin-1) в UTF-8 з нормалізацією
    // типографських символів. Публічно, щоб тестовий режим міг перевірити логіку
    // без потоку. Повертає назву визначеного кодування: "ascii", "utf8",
    // "cp1251", "latin1" (або "none"). out завжди завершується '\0'.
    static const char* normalizeIcy(const char* in, char* out, size_t cap);

    // [Prompt 25] ДОДАНО: рівень виходу декодера, дБ — ЦИФРОВЕ послаблення PCM до I2S
    // (вільна функція-хук audio_process_i2s у audio/vu_pcm_hook.cpp через audio/output_trim).
    // Лише послаблення: db обрізається до station_level_cfg::kLevelMinDb..kLevelMaxDb
    // (-24..0), 0 = без змін. Крок 1 дБ; зміна наживо — плавна (≈ десятки мс, без клацань).
    // Можна викликати з будь-якої задачі й до begin(); не чіпає потік і гучність бібліотеки.
    // Рівень НЕ скидається при stop()/перепідключенні: його задає AppController при старті
    // кожної станції.
    static void setOutputTrimDb(int8_t db);

    // [Prompt 25] ДОДАНО: остання встановлена ціль рівня, дБ (початково -6).
    static int8_t outputTrimDb();

    // [Prompt 17] Декодовані PCM-семпли для VU перехоплює вільна функція
    // audio_process_i2s(int32_t*, int16_t, bool*) в audio/vu_pcm_hook.cpp (за VU_PCM_HOOK_STYLE
    // та ENABLE_VU) і передає піки в VuSourceDecodedPcm::publishPeaks(). Публічний
    // інтерфейс AudioPlayer не змінено.
};
