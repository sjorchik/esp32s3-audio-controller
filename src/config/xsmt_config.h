#pragma once

// Таймінги й трасування послідовностей XSMT (PCM5102) — Prompt X1.
// Піни тут не дублюються (pins::kXsmt). Усі часові значення — у мілісекундах.
//
// УСІ значення нижче — ГІПОТЕЗИ, що потребують підбору на залізі (див. hwcheck_x1.md). Жодне з них
// не виміряне: чат не мав доступу до пристрою. Орієнтир із datasheet PCM510xA (із памʼяті, не звірено
// з документом): soft-mute/unmute ≈ 100 відліків LRCK (≈ 2 мс), внутрішній PLL при SCK = GND
// захоплює BCK за одиниці-десятки мс після появи/зміни клоку.

#include <stddef.h>
#include <stdint.h>

// 1 = друк "[XSMT] t=<мс> <подія>" і консольні команди xsmt.cfg / xsmt.set (читає loop() у main.cpp).
// 0 = увесь код трасування й консолі компілюється геть; таймінги стають constexpr (нульова вартість).
// За замовчуванням 1 на час налаштування; після підбору власник ставить 0.
#define XSMT_TRACE 0

namespace xsmt_cfg {

// Пауза між XSMT LOW (soft-mute) і будь-якою зміною клоку / зупинкою потоку. Має перекривати рампу
// soft-mute ЦАП. Гіпотеза: 20 мс (було 20 мс як kXsmtMuteSettleMs у line_path; у плеєрі не було — XSMT
// мʼютився ПІСЛЯ stopSong()).
constexpr uint32_t kMuteLeadMs = 20;

// Пауза між стабільним клоком I2S0 і XSMT HIGH. Радіо: відлік від останньої зміни Audio::getSampleRate()
// (і не раніше стану Playing). Прохід: від застосування 48 кГц на I2S0 (раніше: 20 мс + 6 settle-блоків
// ≈ 16 мс). Гіпотеза: 40 мс (час захоплення PLL ЦАП + запас).
constexpr uint32_t kClockSettleMs = 40;

// Пауза між XSMT HIGH і зняттям ЦИФРОВОГО мʼюту проходу (DspVolume). Лише для проходу: цифровим мʼютом
// радіо (radio_volume) керує AppController, плеєр його не чіпає. Гіпотеза: 10 мс (≈ 2× рампа unmute ЦАП).
constexpr uint32_t kUnmuteHoldMs = 10;

// Пауза між зупинкою одного джерела клоку (радіо Idle) і запуском іншого (фіксація 48 кГц на I2S0).
// Замінює kRadioDrainMs = 600 мс (XSMT на той момент уже LOW, хвіст буфера нічого не відтворює).
// Гіпотеза: 50 мс.
constexpr uint32_t kClockSwitchGapMs = 50;

// Радіо: якщо Audio::getSampleRate() лишається 0 — скільки чекати від входу в Playing до XSMT HIGH.
constexpr uint32_t kRateWaitMaxMs = 1000;

// [X1b/X1c] Прохід: PCM1808 після зміни клоку I2S0 скидається: ≈ 210..250 мс видає нулі, потім ВІДРАЗУ стрибок постійної
// складової ≈ -21 dBFS, що спадає експоненційно (τ ≈ 170 мс — власний ВЧ-фільтр АЦП ≈ 1 Гц) і сягає ≈ -60 dBFS через ≈ 1 с
// (ВИМІРЯНО пробою на залізі, лог власника 2026-10-25). Саме цей стрибок на розмʼюті й клацає (і б'є VU).
// kAdcSettleMs — скільки XSMT лишається LOW, а цифровий мʼют тримається, ПІСЛЯ появи перших даних АЦП (onset; див. нижче).
// Орієнтир: -21 dBFS * (спад 2,6 дБ / 50 мс) -> ≈ -73 dBFS через 1000 мс. Можна зменшувати, поки клацання не зʼявиться.
constexpr uint32_t kAdcSettleMs = 500;  // підібрано власником на залізі (xsmt.set adc_settle 500); було 1000

// Перші kAdcIgnoreMs після enable RX дані ігноруються при пошуку onset (там одиночний викид при зміні клоку).
constexpr uint32_t kAdcIgnoreMs = 100;
// Якщо onset не знайдено за стільки мс від enable — вважати, що він настав зараз (запасний вихід).
constexpr uint32_t kAdcOnsetMaxMs = 1500;
// Onset = перший відлік 32-бітного RX з |x| > порога (≈ -90 dBFS; шум нулів ≈ -132 dBFS).
constexpr int32_t kAdcOnsetThreshold = 67900;

// Верхня межа значення xsmt.set.
constexpr uint32_t kParamMaxMs = 5000;

// Консоль (лише XSMT_TRACE): період опитування Serial у loop() і розміри рядків.
constexpr uint32_t kConsolePollMs = 20;
constexpr size_t   kConsoleLineMax = 64;
constexpr size_t   kEventLineMax = 160;

static_assert(kParamMaxMs >= kClockSwitchGapMs && kParamMaxMs >= kRateWaitMaxMs, "kParamMaxMs too small");
static_assert(kAdcIgnoreMs < kAdcOnsetMaxMs && kAdcOnsetThreshold > 0, "ADC onset constants");
static_assert(kConsoleLineMax >= 32 && kEventLineMax >= 64, "console buffers too small");

}  // namespace xsmt_cfg

// ---------------------------------------------------------------------------
// Доступ до значень: при XSMT_TRACE = 1 — змінні в RAM (xsmt.set), при 0 — constexpr.
// ---------------------------------------------------------------------------
namespace xsmt {

#if XSMT_TRACE
enum Param : uint8_t { kPMuteLead, kPClockSettle, kPUnmuteHold, kPClockSwitchGap, kPRateWaitMax, kPAdcSettle, kParamCount };
extern volatile uint32_t g_values[kParamCount];  // визначено в audio/xsmt_trace.cpp
inline uint32_t muteLeadMs() { return g_values[kPMuteLead]; }
inline uint32_t clockSettleMs() { return g_values[kPClockSettle]; }
inline uint32_t unmuteHoldMs() { return g_values[kPUnmuteHold]; }
inline uint32_t clockSwitchGapMs() { return g_values[kPClockSwitchGap]; }
inline uint32_t rateWaitMaxMs() { return g_values[kPRateWaitMax]; }
inline uint32_t adcSettleMs() { return g_values[kPAdcSettle]; }
#else
constexpr uint32_t muteLeadMs() { return xsmt_cfg::kMuteLeadMs; }
constexpr uint32_t clockSettleMs() { return xsmt_cfg::kClockSettleMs; }
constexpr uint32_t unmuteHoldMs() { return xsmt_cfg::kUnmuteHoldMs; }
constexpr uint32_t clockSwitchGapMs() { return xsmt_cfg::kClockSwitchGapMs; }
constexpr uint32_t rateWaitMaxMs() { return xsmt_cfg::kRateWaitMaxMs; }
constexpr uint32_t adcSettleMs() { return xsmt_cfg::kAdcSettleMs; }
#endif

}  // namespace xsmt
