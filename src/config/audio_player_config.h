#pragma once

// Константи модуля audio/audio_player (ESP32-audioI2S, інтернет-радіо, XSMT,
// ICY-метадані, перепідключення) та його тестового режиму.
// Піни тут не дублюються: вони лише в config/pins.h (pins::kI2sBclk / kI2sWs /
// kI2sDout / kXsmt). Усі часові значення — у мілісекундах, якщо не вказано інше.

#include <stddef.h>
#include <stdint.h>

#include "config/input_config.h"
#include "config/ir_config.h"

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Тестовий режим плеєра: Serial-команди (audio/audio_player_test.*).
// Вимкнути (0), коли зʼявиться AppController.
// УВАГА: AudioProcTest і AudioPlayerTest читають той самий Serial, тому при
// AUDIO_PLAYER_TEST == 1 main.cpp НЕ запускає AudioProcTest (див. main.cpp).
// Гучність процесора в цьому режимі — команда `vol`.
#define AUDIO_PLAYER_TEST 1

// Serial-лог плеєра: зміни стану, мʼют, перепідключення ("[PLAYER] ...").
#define AUDIO_PLAYER_DEBUG 1

// Друкувати в Serial кожен рядок, який бібліотека віддає в audio_info.
// Дуже багатослівно, але корисно при першому запуску (видно "stream ready",
// HTTP-коди, помилки). Вимкнути після налагодження.
#define AUDIO_PLAYER_LOG_LIBRARY 1

// Стиль callback-ів ESP32-audioI2S — ЗАЛЕЖИТЬ ВІД ВЕРСІЇ БІБЛІОТЕКИ (звірити!):
//   1 = глобальні функції audio_info(), audio_showstation(), audio_showstreamtitle(),
//       audio_bitrate(), audio_eof_stream() ... (класичний API 2.x / ранні 3.x);
//   2 = статичний Audio::audio_info_callback з Audio::msg_t (нові версії 3.x, git master).
// Якщо стиль не той: 2 дає ПОМИЛКУ КОМПІЛЯЦІЇ (гучно), 1 на новій бібліотеці
// компілюється, але callback-и мовчки не викликаються (метаданих не буде).
#define AUDIO_PLAYER_CB_STYLE 2

// 1 = у вашій версії бібліотеки є Audio::setBufsize(ram, psram) — тоді розмір
// буфера задають kStreamBuf*Bytes нижче. 0 = такого методу нема (перевірено:
// у git-версії, з якою зібрано проєкт, його немає) — діє розмір буфера за
// замовчуванням самої бібліотеки, а kStreamBuf*Bytes ігноруються.
#define AUDIO_PLAYER_HAS_SETBUFSIZE 0

// 1 = наша задача сама викликає Audio::loop() (як вимагає завдання).
// 0 = якщо ваша версія бібліотеки сама створює внутрішню задачу декодування
// (тоді подвійний виклик loop() дав би гонку) — тоді наша задача лише веде
// автомат станів. Звірити в Audio.h/Audio.cpp.
#define AUDIO_PLAYER_CALL_LOOP 1

namespace player_cfg {

// ---------------------------------------------------------------------------
// Задача плеєра (FreeRTOS)
// ---------------------------------------------------------------------------
// Ядро 1 = аудіо (розділ 5 MASTER SPEC). На ядрі 1 більше нічого не запускаємо
// (Arduino loopTask там має пріоритет 1 і майже завжди спить).
constexpr int      kTaskCore       = 1;

// Пріоритет 5: наступний вільний над введенням (input_cfg::kBtnTaskPriority =
// kEncTaskPriority = ir_cfg::kTaskPriority = 4). Аудіо — найчутливіше до
// затримок, тому воно вище за введення. Інші задачі: AppController ≈ 3,
// UI = 2, тестові = 1, мережа/веб ≈ 1. Системні задачі IDF (Wi-Fi, lwIP
// tcpip_thread, ipc) мають значно вищі пріоритети й витісняють нас — так і треба,
// інакше не приймався б потік. Задача кожен цикл віддає процесор (kLoopDelayMs),
// тож loopTask (1) і IDLE1 (0) на ядрі 1 не голодують.
constexpr uint8_t  kTaskPriority   = 5;

// Стек, байт. Декодери (AAC/FLAC/Vorbis), TLS-хендшейк https-потоків і
// Serial.printf працюють у контексті Audio::loop() — потрібен запас.
constexpr uint32_t kTaskStackBytes = 16384;

// Пауза між викликами Audio::loop(). Бібліотека вже має внутрішній буфер, тож
// 1 мс (один тік при 1000 Гц) достатньо, щоб віддати процесор іншим задачам.
constexpr uint32_t kLoopDelayMs    = 1;

// Скільки чекати на мʼютекс стану/метаданих (тримається лише на копіювання).
constexpr uint32_t kMutexTimeoutMs = 20;

// ---------------------------------------------------------------------------
// Бібліотека ESP32-audioI2S
// ---------------------------------------------------------------------------
// Гучність бібліотеки ЗАВЖДИ максимальна й незмінна: гучність керується
// виключно AudioProcessor. Шкала бібліотеки за замовчуванням 0..21; більше
// значення бібліотека сама обрізає до свого максимуму.
constexpr uint8_t  kLibVolume      = 21;

// Внутрішній буфер потоку (Audio::setBufsize). Компроміс:
//  + більший буфер = стійкіше до мережевих затримок (400000 байт ≈ 25 с при
//    128 кбіт/с, ≈ 10 с при 320 кбіт/с);
//  − довший старт/перемикання станції (буфер треба наповнити), більше PSRAM
//    (8 МБ загалом; дисплей бере ≈ 217 КБ).
// Буфер у PSRAM використовується, якщо PSRAM знайдено; інакше — лише RAM-буфер.
// Значення виставляються ЯВНО додатними числами, бо семантика 0/-1 у setBufsize
// залежить від версії бібліотеки.
constexpr int      kStreamBufRamBytes   = 20480;
constexpr int      kStreamBufPsramBytes = 400000;

// Таймаути підключення бібліотеки (Audio::setConnectionTimeout): http / https.
// Типові значення бібліотеки (≈250 / 2700 мс) закороткі для повільного DNS.
constexpr uint16_t kHttpTimeoutMs  = 3000;
constexpr uint16_t kHttpsTimeoutMs = 5000;

// ---------------------------------------------------------------------------
// Софт-мʼют PCM5102 (XSMT, active-low)
// ---------------------------------------------------------------------------
constexpr uint8_t  kXsmtMutedLevel   = 0;  // LOW = мʼют
constexpr uint8_t  kXsmtUnmutedLevel = 1;  // HIGH = звук

// Скільки безперервно має тривати Playing, перш ніж зняти мʼют (і скинути
// backoff). Захищає від мʼют/розмʼют на кожен короткий збій.
constexpr uint32_t kUnmuteStableMs   = 3000;

// ---------------------------------------------------------------------------
// Перепідключення (exponential backoff, кількість спроб НЕ обмежена)
// ---------------------------------------------------------------------------
constexpr uint32_t kReconnectInitialMs = 1000;
constexpr uint32_t kReconnectMaxMs     = 30000;
constexpr uint32_t kReconnectFactor    = 2;

// Декодер/зʼєднання вважається зупиненим (isRunning()==false), якщо це триває
// довше за цей час, поки ми очікуємо відтворення.
constexpr uint32_t kNotRunningGraceMs  = 1500;

// Скільки максимум чекати наповнення буфера (стан Buffering), перш ніж
// вважати потік завислим і перепідключитись.
constexpr uint32_t kBufferingTimeoutMs = 20000;

// Запасний критерій «потік грає», якщо не прийшов ні bitrate-callback, ні
// текст kStreamReadyText: минуло стільки мс від підключення, isRunning() і
// буфер не порожній (>= kBufferLowPercent).
constexpr uint32_t kAssumePlayingMs    = 5000;

// Текст у audio_info, який бібліотека друкує, коли буфер наповнено й почалось
// відтворення (у версіях 2.x/3.x це "stream ready" — звірити).
constexpr const char* kStreamReadyText = "stream ready";

// Моніторинг заповнення буфера: період опитування та пороги гістерезису, %.
// Нижче kBufferLowPercent (після стабільного Playing) = недовантаження →
// Buffering + лічильник; повернення в Playing при >= kBufferRecoverPercent.
constexpr uint32_t kBufferPollMs         = 250;
constexpr uint8_t  kBufferLowPercent     = 3;
constexpr uint8_t  kBufferRecoverPercent = 10;

// ---------------------------------------------------------------------------
// Рядки й метадані
// ---------------------------------------------------------------------------
// Розміри збігаються з AppStateData (stationName[64], trackTitle[128]) і
// Station.url[192] (розділ 12 MASTER SPEC).
constexpr size_t   kUrlMax     = 192;
constexpr size_t   kStationMax = 64;
constexpr size_t   kTitleMax   = 128;

// Скільки байт сирого ICY-рядка обробляємо (надлишок відкидається).
constexpr size_t   kIcyRawMax  = 512;

// true: після перекодування у UTF-8 типографські символи (— – “ ” ‘ ’ … №)
// замінюються ASCII-аналогами, а символи поза покриттям шрифта UI
// (ASCII, Latin-1, Latin Ext-A, U+0400..045F, Ґ ґ) — на '?' (розділ 10).
constexpr bool     kNormalizeForFont = true;

// ---------------------------------------------------------------------------
// Тестовий режим (AUDIO_PLAYER_TEST)
// ---------------------------------------------------------------------------
// Ядро 0, пріоритет 1 (як audio_cfg::kTestTaskPriority): нижче за введення (4),
// AppController (≈3) і UI (2).
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;
constexpr uint32_t kTestTaskStackBytes = 6144;
constexpr uint32_t kTestPollMs         = 20;

// Максимальна довжина рядка команди разом із '\0' (URL і hex-рядки довгі).
constexpr size_t   kTestLineMax        = 200;

// Тимчасове Wi-Fi ТІЛЬКИ для тесту (WifiManager ще немає). Порожній SSID =
// не підключатись автоматично; підключення командою `wifi <ssid> <pass>`.
// НЕ комітьте сюди реальний пароль.
constexpr const char* kTestWifiSsid = "";
constexpr const char* kTestWifiPass = "";

// Тестові станції. URL ПУБЛІЧНІ, але їхню працездатність я не перевіряв
// (доступу до мережі не було): стріми можуть змінити адресу, кодек чи піти на
// https. Якщо станція не грає — підставте власний робочий URL.
constexpr const char* kTestStationNames[] = {
    "Radio ROKS",
    "Radio Gold",
    "Hit FM",
};
constexpr const char* kTestStationUrls[] = {
    "https://online.radioroks.ua/RadioROKS",
    "https://online.radioplayer.ua/RadioGold",
    "https://online.hitfm.ua/HitFM",
};
constexpr size_t   kTestStationCount =
    sizeof(kTestStationUrls) / sizeof(kTestStationUrls[0]);

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kTaskCore == 1, "audio runs on core 1 (MASTER SPEC, section 5)");
static_assert(kTaskPriority > input_cfg::kBtnTaskPriority &&
                  kTaskPriority > input_cfg::kEncTaskPriority &&
                  kTaskPriority > ir_cfg::kTaskPriority,
              "audio priority must be above input tasks (4)");
static_assert(kLoopDelayMs >= 1, "kLoopDelayMs must be >= 1 (task must yield)");
static_assert(kReconnectInitialMs > 0 && kReconnectInitialMs <= kReconnectMaxMs,
              "backoff: initial must be in (0, max]");
static_assert(kReconnectFactor >= 1, "backoff factor must be >= 1");
static_assert(kBufferLowPercent < kBufferRecoverPercent, "buffer hysteresis inverted");
static_assert(kBufferRecoverPercent <= 100, "percent out of range");
static_assert(kStreamBufRamBytes > 0 && kStreamBufPsramBytes > 0,
              "buffer sizes must be explicit positive numbers");
static_assert(kUrlMax == 192, "must match Station.url[192]");
static_assert(kStationMax == 64 && kTitleMax == 128, "must match AppStateData");
static_assert(kIcyRawMax >= kTitleMax, "raw ICY limit smaller than title buffer");
static_assert(kTestStationCount >= 2 && kTestStationCount <= 3, "2..3 test stations");
static_assert(sizeof(kTestStationNames) / sizeof(kTestStationNames[0]) == kTestStationCount,
              "test station names/urls count mismatch");
static_assert(kTestLineMax >= 64, "kTestLineMax too small");

}  // namespace player_cfg