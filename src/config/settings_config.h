#pragma once

// Константи модуля core/settings (NVS) і core/settings_test.
// Значення за замовчуванням самих налаштувань тут не дублюються:
// вони беруться з config/defaults.h, config/display_config.h
// (kDefaultFlipped) та audio_cfg::kTestProcType (див. core/settings.cpp).

#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-тестовий режим Settings (команди з префіксом `set.`, див. settings_test.h).
// Вимкнути (0), коли зʼявиться AppController / веб-інтерфейс налаштувань.
#define SETTINGS_TEST 1

// AudioProcTest і AudioPlayerTest читають той самий Serial, і байти між двома
// читачами ділились би випадково (префікс команди цього не лікує — байт
// забирає той, хто встиг першим). Тому за замовчуванням SettingsTest НЕ стартує,
// поки активний будь-який із них. 1 = стартувати все одно (на свій ризик:
// частина рядків буде «з'їдена» іншим тестом). Див. розділ «Відомі обмеження».
#define SETTINGS_TEST_SHARE_SERIAL 0

namespace settings_cfg {

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
// Namespace спільний з IrRc5 (його ключ — ir_cfg::kNvsMapKey = "ir_map").
// Наш ключ мусить відрізнятися; довжина ключа NVS — не більше 15 символів.
constexpr const char* kNvsNamespace = "audioctl";
constexpr const char* kNvsBlobKey   = "settings";

// Версія формату blob-а. Збільшувати при БУДЬ-ЯКІЙ зміні порядку/типів полів
// Settings (зміна лише розміру ловиться ще й перевіркою sizeof). Невідповідна
// версія → значення за замовчуванням (міграції немає).
constexpr uint8_t kFormatVersion = 1;

// ---------------------------------------------------------------------------
// Відкладений запис (дебаунс)
// ---------------------------------------------------------------------------
// requestSave() лише ставить прапорець. Фактичний запис — коли від ОСТАННЬОГО
// requestSave() минуло kSaveDebounceMs, АБО від ПЕРШОГО з серії минуло
// kSaveMaxDelayMs (щоб нескінченний потік змін не відкладав запис вічно).
constexpr uint32_t kSaveDebounceMs = 2000;
constexpr uint32_t kSaveMaxDelayMs = 10000;
static_assert(kSaveMaxDelayMs >= kSaveDebounceMs, "max delay must cover debounce");

// Як часто фонова задача перевіряє прапорець (ціна перевірки — один мʼютекс).
constexpr uint32_t kFlushPollMs = 250;

// Задача фонового запису. Ядро 0, пріоритет 1 (як мережа/веб у таблиці
// розділу 5 MASTER SPEC): запис у флеш не повинен конкурувати з UI й введенням.
constexpr int      kFlushTaskCore       = 0;
constexpr uint8_t  kFlushTaskPriority   = 1;
constexpr uint32_t kFlushTaskStackBytes = 4096;

// Скільки чекати мʼютекс Settings. Запис у NVS зазвичай кілька мс, але під час
// збирання сміття сторінок може тривати сотні мс.
constexpr uint32_t kMutexTimeoutMs = 500;

// ---------------------------------------------------------------------------
// SettingsTest
// ---------------------------------------------------------------------------
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;
constexpr uint32_t kTestTaskStackBytes = 4096;
constexpr uint32_t kTestPollMs         = 20;
constexpr uint16_t kTestLineMax        = 96;

// `set.ramp`: імітація ramp гучності (kTestRampSteps викликів requestSave()
// з кроком kTestRampStepMs — за замовчуванням 100 × 20 мс = 2 с, 50 разів/с).
constexpr uint16_t kTestRampSteps  = 100;
constexpr uint32_t kTestRampStepMs = 20;

}  // namespace settings_cfg
