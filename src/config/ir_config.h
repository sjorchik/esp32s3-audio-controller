#pragma once

// Константи модуля input/ir_rc5 (RC5/RC5X, навчання, NVS).
// Усі часові значення — у мілісекундах або мікросекундах (вказано в імені).
// Піни тут не дублюються: вони лише в config/pins.h (pins::kIrIn).

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог кожного валідного кадру: "[IR] addr=0 cmd=16 toggle=1 rc5x=0"
// та кроків режиму навчання. Окремо від INPUT_DEBUG.
#define IR_DEBUG 1

namespace ir_cfg {

// ---------------------------------------------------------------------------
// Задача приймання (FreeRTOS)
// ---------------------------------------------------------------------------
// Ядро 0, пріоритет 4 = як у кнопок/енкодера (розділ 5 MASTER SPEC):
// вище за AppController/UI, нижче за аудіо (ядро 1). Задача майже весь час
// блокується на черзі RMT, тож не заважає.
constexpr int      kTaskCore       = 0;
constexpr uint8_t  kTaskPriority   = 4;
// Стек з запасом: у цій задачі виконується запис у NVS (Preferences) і Serial.printf.
constexpr uint32_t kTaskStackBytes = 6144;

// Період пробудження задачі без кадрів (перевірка таймаутів навчання).
constexpr uint32_t kTaskTickMs     = 50;

// Довжина черги ISR -> задача (одночасно армований лише один прийом).
constexpr uint8_t  kRxQueueLen     = 2;

// ---------------------------------------------------------------------------
// RMT RX
// ---------------------------------------------------------------------------
// Роздільність 1 МГц: 1 тік = 1 мкс (декодер трактує тривалості як мкс).
constexpr uint32_t kRmtResolutionHz    = 1000000;

// Блок памʼяті каналу RX (ESP32-S3: 48 символів на блок).
constexpr size_t   kRmtMemBlockSymbols = 48;

// Буфер прийому в символах RMT (має бути >= kRmtMemBlockSymbols).
constexpr size_t   kRmtRxBufSymbols    = 64;

// Апаратний фільтр коротких імпульсів, нс (голки). Апаратна межа ≈ 3 мкс.
constexpr uint32_t kRmtMinSignalNs     = 1000;

// Кадр вважається завершеним, якщо рівень не змінюється так довго, мкс.
// Має бути більшим за найдовший імпульс усередині кадру (2 піврівні ≈ 1778 мкс).
constexpr uint32_t kRmtIdleThresholdUs = 4000;

// Рівень піна, що означає «несуча присутня». VS1838B інвертує: LOW = 0.
constexpr uint8_t  kMarkLevel          = 0;

// Максимум сегментів (змін рівня) у кадрі, які декодер готовий розібрати.
constexpr size_t   kMaxSegments        = 32;

// ---------------------------------------------------------------------------
// Таймінги RC5
// ---------------------------------------------------------------------------
// Тривалість півбіта, мкс (RC5: 889 мкс, повний біт 1778 мкс).
constexpr uint32_t kHalfBitUs             = 889;

// Допуск на тривалість (n × півбіт), ±%. Несуча 36 кГц при 38 кГц-приймачі
// дає нижчу чутливість і «пливе» довжину імпульсів — допуск ширший за типовий.
// Має бути < 33, щоб діапазони 1 і 2 півбітів не перекривалися.
constexpr uint32_t kHalfBitTolerancePercent = 30;

// Корекція асиметрії VS1838B, мкс: додається до тривалості несучої (LOW)
// і віднімається від паузи. Підбирається за фактичними вимірами; 0 = вимкнено.
constexpr int32_t  kMarkBiasUs            = 0;

// ---------------------------------------------------------------------------
// Розрізнення натискання / утримання
// ---------------------------------------------------------------------------
// Кадри при утриманні йдуть кожні ≈114 мс. Той самий (addr, cmd, toggle)
// з паузою не більше цього порога = повтор; довша пауза = нове натискання.
constexpr uint32_t kRepeatGapMs   = 250;

// Скільки утримувати, перш ніж почати слати repeat=true
// (VOL_UP, VOL_DOWN, UP, DOWN, LEFT, RIGHT). Далі — кожен кадр (≈114 мс).
constexpr uint32_t kRepeatDelayMs = 500;

// ---------------------------------------------------------------------------
// Режим навчання
// ---------------------------------------------------------------------------
// Таймаут очікування першого й повторного натискання (кожного окремо).
constexpr uint32_t kLearnTimeoutMs         = 8000;

// Скільки чекати на confirmOverwrite() після статусу Conflict.
constexpr uint32_t kLearnConflictTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
constexpr const char* kNvsNamespace = "audioctl";
constexpr const char* kNvsMapKey    = "ir_map";   // ≤ 15 символів

// Версія бінарного формату blob (перший байт). Змінити при зміні формату
// або при перестановці значень Action.
constexpr uint8_t  kMapFormatVersion = 1;

// Максимум записів у мапі (28 дій у Action + запас).
constexpr uint8_t  kMaxMapEntries    = 40;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kRmtResolutionHz == 1000000, "decoder assumes 1 tick = 1 us");
static_assert(kRmtRxBufSymbols >= kRmtMemBlockSymbols, "RX buffer must cover one RMT memory block");
static_assert(kHalfBitTolerancePercent > 0 && kHalfBitTolerancePercent < 33,
              "tolerance must keep 1x and 2x half-bit ranges apart");
static_assert(kRmtIdleThresholdUs > (2 * kHalfBitUs * (100 + kHalfBitTolerancePercent)) / 100,
              "idle threshold must exceed the longest in-frame pulse");
static_assert(kRmtIdleThresholdUs <= 32000, "idle threshold exceeds RMT register range");
static_assert(kMaxSegments >= 28, "need room for a full RC5 frame");
static_assert(kRepeatGapMs >= 150, "repeat gap must exceed the ~114 ms RC5 frame period");
static_assert(kTaskTickMs > 0, "kTaskTickMs must be > 0");
static_assert(kMaxMapEntries >= 28 && kMaxMapEntries <= 255, "map size out of range");

}  // namespace ir_cfg
