#pragma once

// Константи модуля stations/station_store (список станцій у LittleFS + RAM,
// імпорт M3U/PLS/JSON) та його тестового режиму.
// Шляхи — відносно кореня LittleFS (змонтовано в main.cpp). Часові значення — мс.

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог модуля ("[STATIONS] ...").
#define STATION_STORE_DEBUG 1

// Тестовий режим: Serial-команди st.list / st.export / st.reset / st.import
// (stations/station_store_test.*).
// УВАГА: кілька тестових модулів на одному Serial ділять байти між собою
// (кожен байт дістається лише одному читачу), тому за замовчуванням тест НЕ
// стартує, поки активний AUDIO_PROC_TEST / AUDIO_PLAYER_TEST / SETTINGS_TEST.
// 1 у STATION_STORE_TEST_SHARE_SERIAL знімає це обмеження (на свій ризик).
#define STATION_STORE_TEST 1
#define STATION_STORE_TEST_SHARE_SERIAL 0

namespace station_store_cfg {

// ---------------------------------------------------------------------------
// Зберігання
// ---------------------------------------------------------------------------
// Постійне сховище. Формат: {"version":N,"stations":[{"name":"..","url":".."},..]}
constexpr const char* kStoragePath = "/stations.json";

// Запис — через тимчасовий файл (path + kTmpSuffix) і rename: обрив живлення
// посеред запису не псує попередній файл.
constexpr const char* kTmpSuffix = ".tmp";

// Пошкоджений / іншої версії файл сховища перейменовується в path + kBackupSuffix
// перед повторним seed (одна резервна копія, попередня перезаписується).
constexpr const char* kBackupSuffix = ".bad";

// Версія формату. Не збігається -> попередження в лог, файл у резерв, seed.
constexpr uint32_t kFormatVersion = 1;

// ---------------------------------------------------------------------------
// Ємність
// ---------------------------------------------------------------------------
// Station = 2 (id) + 64 (name) + 192 (url) = 258 байт.
// 256 станцій = 66 048 байт (≈ 64,5 КБ). Масив у PSRAM (8 МБ), не у внутрішній
// SRAM: після Wi-Fi/lwIP/TLS (https-потоки)/16 КБ стеку аудіо вільної SRAM
// лишається небагато, а 64 КБ там — відчутна частка. На PSRAM це <1 %.
// Для реального користувача 256 — з великим запасом (типово десятки станцій).
constexpr size_t kMaxStations = 256;

// Розміри полів Station (мають збігатися зі struct Station і AppStateData).
constexpr size_t kNameMax = 64;
constexpr size_t kUrlMax  = 192;

// ---------------------------------------------------------------------------
// Імпорт
// ---------------------------------------------------------------------------
// Буфер рядка M3U/PLS, байт (на стеку). Довші рядки обрізаються; URL, що не
// вмістився, пропускається (обрізаний URL марний).
constexpr size_t kLineMax = 512;

// Максимальний розмір файлу, який імпортуємо/читаємо, байт. 256 станцій при
// ≤ 300 байт на запис ≈ 77 КБ; з запасом на M3U з довгими #EXTINF.
constexpr size_t kMaxFileBytes = 128 * 1024;

// ---------------------------------------------------------------------------
// Мʼютекси
// ---------------------------------------------------------------------------
// Мʼютекс даних тримається лише на копіювання одного запису (читачі) або
// на memcpy списку (імпорт) — десятки мікросекунд.
constexpr uint32_t kMutexTimeoutMs   = 50;
// Мʼютекс операцій з файлами (імпорт/експорт): запис у flash триває довго.
constexpr uint32_t kIoMutexTimeoutMs = 10000;
// [Prompt 14] Те саме для add/update/remove/move (їх викликає обробник HTTP у задачі
// async_tcp, якій не можна зависати надовго): коротший тайм-аут -> false.
constexpr uint32_t kEditIoMutexTimeoutMs = 2000;

// ---------------------------------------------------------------------------
// Тестовий режим (STATION_STORE_TEST)
// ---------------------------------------------------------------------------
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;     // як інші тестові задачі
constexpr uint32_t kTestTaskStackBytes = 8192;  // імпорт виконується в цій задачі
constexpr uint32_t kTestPollMs         = 20;
// Максимальна довжина рядка команди з '\0' (st.import <json> буває довгим).
constexpr size_t   kTestLineMax        = 1536;
constexpr const char* kTestImportPath  = "/st_test_import.json";
constexpr const char* kTestExportPath  = "/st_test_export.json";

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kMaxStations >= 8 && kMaxStations <= 1024, "kMaxStations out of sane range");
static_assert(kNameMax == 64 && kUrlMax == 192, "must match Station / AppStateData");
static_assert(kLineMax > kUrlMax + 2, "line buffer must hold a full URL");
static_assert(kMaxFileBytes >= 4096, "kMaxFileBytes too small");
static_assert(kTestLineMax >= 128, "kTestLineMax too small");
static_assert(kFormatVersion >= 1, "version starts at 1");
static_assert(kEditIoMutexTimeoutMs >= 100 && kEditIoMutexTimeoutMs <= kIoMutexTimeoutMs,
              "edit IO timeout out of range");

}  // namespace station_store_cfg
