#pragma once

// Константи модуля core/settings (NVS) і core/settings_test.
// Значення за замовчуванням самих налаштувань тут не дублюються:
// вони беруться з config/defaults.h, config/display_config.h
// (kDefaultFlipped) та audio_cfg::kTestProcType (див. core/settings.cpp).
// Виняток: kDefaultInputGain (у defaults.h такої константи немає, а defaults.h не чіпаємо).

#include <stdint.h>

#include "config/defaults.h"  // [Prompt 39] defaults::kInputCount (маска входів)

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
// версія → значення за замовчуванням, КРІМ єдиної підтримуваної міграції
// kLegacyFormatVersion -> kFormatVersion (див. SettingsStore::load()).
// [Prompt 21b] 1 -> 2: глобальні bass/treble/balance/loudness/lastVolume замінено
// профілями по входах (Settings::profiles).
// [Prompt 30] 2 -> 3: у кінець Settings додано eqGainsDb (пресет еквалайзера радіо).
// [Prompt 39] 3 -> 4: у кінець Settings додано inputEnabledMask (дозволені входи).
constexpr uint8_t kFormatVersion = 4;

// [Prompt 21b] Формат, який ще вміємо читати й мігрувати (усі профілі входів
// успадковують колишні глобальні значення). Інші версії -> значення за замовчуванням.
constexpr uint8_t kLegacyFormatVersion = 1;

// [Prompt 30] Формат v2 (профілі входів, без еквалайзера): читається й мігрується
// (усе зберігається, еквалайзер = плоский 0 дБ, [Prompt 39] усі входи дозволені).
constexpr uint8_t kPrevFormatVersion = 2;

// [Prompt 39] Формат v3 (з еквалайзером, без маски входів): читається й мігрується
// (усе зберігається, усі входи дозволені).
constexpr uint8_t kPrev3FormatVersion = 3;
static_assert(kLegacyFormatVersion < kPrevFormatVersion && kPrevFormatVersion < kPrev3FormatVersion &&
                  kPrev3FormatVersion < kFormatVersion,
              "format versions must be strictly increasing");

// [Prompt 39] Маска дозволених логічних входів (Settings::inputEnabledMask): біт i = вхід i.
// Усі входи за замовчуванням дозволені; біт 0 (радіо) завжди 1 — вимкнути можна лише входи 1..3.
// Апаратна доступність (AudioProcessorCapabilities::inputCount) від маски не залежить:
// вхід «ефективний» = апаратно є І біт у масці.
static_assert(defaults::kInputCount <= 8, "inputEnabledMask is a single byte");
constexpr uint8_t kInputMaskAll = static_cast<uint8_t>((1u << defaults::kInputCount) - 1u);
constexpr uint8_t kInputMaskRadioBit = 0x01;

// Єдине місце нормалізації маски: зайві біти (≥ kInputCount) скидаються, біт радіо = 1.
constexpr uint8_t normalizeInputMask(uint8_t m) {
    return static_cast<uint8_t>((m & kInputMaskAll) | kInputMaskRadioBit);
}

// [Prompt 21b] Підсилення входу за замовчуванням (сирі апаратні кроки) для нових і
// мігрованих профілів. Раніше gain у NVS не зберігався й після старту дорівнював
// початковому значенню драйвера (0).
constexpr int8_t kDefaultInputGain = 0;

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
