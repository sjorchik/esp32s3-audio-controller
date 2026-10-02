#pragma once

// Константи модулів input/buttons та input/encoder.
// Усі часові значення — у мілісекундах, якщо не вказано інше.
// Піни тут не дублюються: вони лише в config/pins.h.

#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог кожної події кнопок і енкодера: "[BTN] UP short", "[ENC] CW delta=2".
#define INPUT_DEBUG 1

// Тимчасовий вивід усіх подій з EventBus у main.cpp ("[EVT] ...").
// [Prompt 8] ЗМІНЕНО: 1 -> 0. Тепер EventBus читає AppController; два читачі
// однієї черги отримували б різні підмножини подій. Код друку з main.cpp
// прибрано; прапорець лишено (0), щоб не ламати можливі #if в інших файлах.
// Лог подій тепер веде AppController (APP_CONTROLLER_LOG_EVENTS).
#define INPUT_DEMO_PRINT_EVENTS 0

namespace input_cfg {

// ---------------------------------------------------------------------------
// Задачі FreeRTOS
// ---------------------------------------------------------------------------
// Розподіл ядер проекту: ядро 0 = UI, мережа, введення, AppController, веб;
// ядро 1 = аудіо. Введення працює на ядрі 0.
//
// Пріоритети: введення НИЖЧЕ за аудіо, але ВИЩЕ за UI. Тобто пріоритет аудіо-
// задачі має бути > kBtnTaskPriority / kEncTaskPriority, а пріоритет UI-задач
// (AppController, дисплей) — нижчий за них. Задачі введення майже весь час
// сплять (vTaskDelayUntil), тож витісняти UI вони будуть на мікросекунди.
constexpr int      kBtnTaskCore       = 0;
constexpr uint8_t  kBtnTaskPriority   = 4;
constexpr uint32_t kBtnTaskStackBytes = 4096;

constexpr int      kEncTaskCore       = 0;
constexpr uint8_t  kEncTaskPriority   = 4;
constexpr uint32_t kEncTaskStackBytes = 4096;

// ---------------------------------------------------------------------------
// Кнопки
// ---------------------------------------------------------------------------
// Активний рівень: 0 = LOW (кнопка замикає пін на GND, внутрішня підтяжка).
constexpr uint8_t  kBtnPressedLevel   = 0;

// Пауза після pinMode(INPUT_PULLUP), щоб підтяжка встигла встановити рівень.
constexpr uint32_t kBtnPullupSettleMs = 5;

// Період опитування кнопок.
constexpr uint32_t kBtnPollMs         = 5;

// Дебаунс: новий стан приймається, якщо він стабільний не менше цього часу.
constexpr uint32_t kBtnDebounceMs     = 30;

// Довге натискання: подія при утриманні не менше цього часу.
constexpr uint32_t kBtnLongPressMs    = 800;

// Повтор (лише UP/DOWN/LEFT/RIGHT): початкова затримка й період.
constexpr uint32_t kBtnRepeatDelayMs  = 500;
constexpr uint32_t kBtnRepeatPeriodMs = 120;

// Довге натискання для кожної кнопки окремо.
// Якщо вимкнено — кнопка дає лише коротку подію при відпусканні
// (незалежно від тривалості утримання).
constexpr bool kBtnLongPressPower  = true;
constexpr bool kBtnLongPressUp     = false;
constexpr bool kBtnLongPressDown   = false;
constexpr bool kBtnLongPressLeft   = false;
constexpr bool kBtnLongPressRight  = false;
constexpr bool kBtnLongPressOk     = true;
constexpr bool kBtnLongPressEncBtn = true;

// ---------------------------------------------------------------------------
// Перевірка утримання кнопки при старті (Buttons::isHeldAtBoot)
// ---------------------------------------------------------------------------
// Період опитування піна під час перевірки.
constexpr uint32_t kBootSamplePeriodMs  = 10;

// Скільки утримувати OK при старті для скидання Wi-Fi.
constexpr uint32_t kBootWifiResetHoldMs = 3000;

// ---------------------------------------------------------------------------
// Енкодер (PCNT, квадратурний режим)
// ---------------------------------------------------------------------------
// Межі апаратного лічильника. Для акумуляції переповнень драйвер PCNT
// рекомендує якомога більші межі (менше переривань). Максимум 16 біт.
constexpr int kEncPcntLowLimit  = -30000;
constexpr int kEncPcntHighLimit = 30000;

// Glitch filter PCNT, нс. Фільтрує лише електричні голки (апаратний максимум
// ~12 мкс при APB 80 МГц). Дребезг контактів гаситься самим квадратурним
// декодуванням (+1 і -1 взаємно знищуються). 0 = фільтр вимкнено.
constexpr uint32_t kEncGlitchNs = 5000;

// Скільки імпульсів PCNT (повний квадратурний x4) припадає на один detent.
// Типові EC11: 4 (частіше) або 2.
constexpr int  kEncCountsPerDetent = 4;

// Інверсія напрямку: якщо CW дає CCW — поставити true.
constexpr bool kEncInvertDirection = false;

// Період опитування лічильника PCNT.
constexpr uint32_t kEncPollMs = 10;

// Акселерація: якщо між двома подіями обертання в той самий бік минуло менше
// kEncAccelThresholdMs, delta множиться на kEncAccelMultiplier.
constexpr bool     kEncAccelEnabled     = true;
constexpr uint32_t kEncAccelThresholdMs = 50;
constexpr uint8_t  kEncAccelMultiplier  = 3;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kBtnPollMs > 0, "kBtnPollMs must be > 0");
static_assert(kBtnDebounceMs >= kBtnPollMs, "debounce must be >= poll period");
static_assert(kBtnRepeatPeriodMs >= kBtnPollMs, "repeat period must be >= poll period");
static_assert(kBootSamplePeriodMs > 0, "kBootSamplePeriodMs must be > 0");
static_assert(kEncPcntLowLimit < 0 && kEncPcntHighLimit > 0, "PCNT limits must straddle zero");
static_assert(kEncPcntLowLimit >= -32767 && kEncPcntHighLimit <= 32767, "PCNT limits are 16-bit");
static_assert(kEncCountsPerDetent > 0, "kEncCountsPerDetent must be > 0");
static_assert(kEncPollMs > 0, "kEncPollMs must be > 0");
static_assert(kEncAccelMultiplier >= 1, "kEncAccelMultiplier must be >= 1");

}  // namespace input_cfg
