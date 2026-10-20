#pragma once

// Константи модуля core/settings (NVS) і core/settings_test.
// Значення за замовчуванням самих налаштувань тут не дублюються:
// вони беруться з config/defaults.h, config/display_config.h
// (kDefaultFlipped) та config/balance_config.h (див. core/settings.cpp).

#include <stdint.h>

#include "config/defaults.h"  // [Prompt 39] defaults::kInputCount (маска входів)

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-тестовий режим Settings (команди з префіксом `set.`, див. settings_test.h).
// Вимкнути (0), коли зʼявиться AppController / веб-інтерфейс налаштувань.
#define SETTINGS_TEST 0

// AudioPlayerTest і SettingsTest читають той самий Serial, і байти між двома
// читачами ділились би випадково (префікс команди цього не лікує — байт
// забирає той, хто встиг першим). Тому за замовчуванням SettingsTest НЕ стартує,
// поки активний AudioPlayerTest. 1 = стартувати все одно (на свій ризик:
// частина рядків буде «з'їдена» іншим тестом). Див. розділ «Відомі обмеження».
// [Prompt 48a] AudioProcTest вирізано разом з аудіочіпами.
#define SETTINGS_TEST_SHARE_SERIAL 0

namespace settings_cfg {

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
// Namespace спільний з IrRc5 (його ключ — ir_cfg::kNvsMapKey = "ir_map").
// Наш ключ мусить відрізнятися; довжина ключа NVS — не більше 15 символів.
constexpr const char* kNvsNamespace = "audioctl";
constexpr const char* kNvsBlobKey   = "settings";

// Версія формату blob-а. Збільшувати при БУДЬ-ЯКІЙ зміні порядку/типів полів Settings.
// [Prompt 46b] 5 -> 6: 5 логічних входів (defaults::kInputCount = 5): inputNames[5], profiles[5].
// Розмір Settings 160 -> 198 Б, blob 164 -> 202 Б. МІГРАЦІЙ НЕМАЄ: blob іншої версії/довжини ігнорується,
// діють значення за замовчуванням (прошивка ставиться на стертий flash). Ланцюг міграцій v1..v5 видалено.
// [Prompt 48a] 6 -> 7: Settings без processorType (чипи вирізано), 3 логічні входи (inputNames[3],
// profiles[3]), профіль входу {volume, eqGainsDb[5], balanceDb} = 7 Б. Розмір Settings 198 -> 126 Б,
// blob 202 -> 130 Б. БЕЗ МІГРАЦІЇ (валідний лише blob версії 7 з точним розміром; прошивка — на СТЕРТИЙ flash:
// `pio run -t erase`).
constexpr uint8_t kFormatVersion = 7;

// [Prompt 39] Маска дозволених логічних входів (Settings::inputEnabledMask): біт i = вхід i.
// [Prompt 48a] 3 логічні входи (0 радіо, 1 TV Box, 2 Computer): маска = 3 біти, за замовчуванням усі
// дозволені (kInputMaskDefault = 0x07). Біт 0 (радіо) завжди 1 — вимкнути можна лише входи 1..2.
// Апаратної недоступності входів більше немає (чипів нема): вхід «доступний» = дозволений маскою.
static_assert(defaults::kInputCount <= 8, "inputEnabledMask is a single byte");
constexpr uint8_t kInputMaskAll = static_cast<uint8_t>((1u << defaults::kInputCount) - 1u);
constexpr uint8_t kInputMaskRadioBit = 0x01;
// [Prompt 48a] Маска за замовчуванням: усі три входи дозволені (0x07).
constexpr uint8_t kInputMaskDefault = kInputMaskAll;
static_assert(kInputMaskDefault == 0x07, "default input mask = radio + TV Box + Computer");

// Єдине місце нормалізації маски: зайві біти (≥ kInputCount) скидаються, біт радіо = 1.
constexpr uint8_t normalizeInputMask(uint8_t m) {
    return static_cast<uint8_t>((m & kInputMaskAll) | kInputMaskRadioBit);
}

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
