#pragma once

// Константи модуля core/app_controller.
// Піни тут не дублюються (їх немає: AppController не чіпає залізо напряму).
// Усі часові значення — у мілісекундах, якщо не вказано інше.

#include <stdint.h>

#include "config/audio_player_config.h"  // player_cfg::kTaskPriority, ir_cfg, input_cfg
#include "config/display_config.h"      // display_cfg::kTaskPriority
#include "config/input_config.h"        // input_cfg::kBtnTaskPriority

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог рішень контролера: "[APP] input 0 -> 1", "[APP] standby on" ...
#define APP_CONTROLLER_DEBUG 1

// Додатково друкувати кожну прийняту подію: "[APP] evt BUTTON UP".
// Корисно при перевірці розкладки; вимкнути, коли шумить (енкодер дає багато).
#define APP_CONTROLLER_LOG_EVENTS 1

namespace app_controller_cfg {

// ---------------------------------------------------------------------------
// Задача FreeRTOS
// ---------------------------------------------------------------------------
// Ядро 0. Пріоритет 3: нижче за введення (4) і аудіо (5), вище за UI (2).
constexpr int      kTaskCore       = 0;
constexpr uint8_t  kTaskPriority   = 3;
constexpr uint32_t kTaskStackBytes = 6144;

// Тайм-аут очікування події, коли нічого не відбувається / коли йде перехід
// (Settle, Ramp, утримання мʼюту gain) і таймери треба перевіряти частіше.
constexpr uint32_t kIdlePollMs   = 50;
constexpr uint32_t kActivePollMs = 10;

// Період синхронізації AudioPlayer -> AppState (стан потоку, метадані).
constexpr uint32_t kSyncPeriodMs = 200;

// Скільки чекати на внутрішній мʼютекс контролера (захист handleEvent/tick).
constexpr uint32_t kLockTimeoutMs = 200;

// ---------------------------------------------------------------------------
// Переходи: мʼют -> зміна входу/станції -> розмʼют -> ramp
// ---------------------------------------------------------------------------
// Пауза між командами зміни (вхід/станція/пробудження) і розмʼютом атенюаторів.
constexpr uint32_t kUnmuteDelayMs = 250;

// Ramp гучності після розмʼюту: від volumeMin до цілі кроками.
constexpr uint32_t kRampStepMs = 20;
constexpr int      kRampStep   = 2;   // у одиницях шкали гучності UI (0..100)

// [Prompt 31] Автоповернення цілі регулювання енкодера на гучність: скільки мс без подій
// енкодера (обертання, клік, утримання), після чого ціль (бас/дискант/баланс/gain) знову
// стає гучністю.
constexpr uint32_t kAdjustTimeoutMs = 5000;

// Скільки тримати мʼют атенюаторів після зміни gain (апаратний стрибок рівня).
constexpr uint32_t kGainMuteHoldMs = 80;

// Через скільки повторити setMute(), якщо I2C-транзакція не вдалась.
constexpr uint32_t kMuteRetryMs = 500;

// ---------------------------------------------------------------------------
// Розкладка керування
// ---------------------------------------------------------------------------
// Крок гучності на один detent енкодера / одну дію VOL_UP, VOL_DOWN
// (шкала UI 0..100; з акселерацією енкодера крок множиться).
constexpr int8_t kVolumeStep = 1;

// Напрямок зміни входу/станції (+1 = наступний за номером, -1 = попередній).
// UP -> попередній вхід, DOWN -> наступний; LEFT -> попередня станція,
// RIGHT -> наступна. Інвертувати тут, якщо звичка інша.
constexpr int8_t kInputUpStep       = -1;
constexpr int8_t kInputDownStep     = +1;
constexpr int8_t kStationLeftStep   = -1;
constexpr int8_t kStationRightStep  = +1;

// true: після старту лишатись у Standby (мʼют, потік не запускається);
// false: відновити останній вхід/станцію й увімкнутись з ramp.
constexpr bool kBootInStandby = false;

// ---------------------------------------------------------------------------
// [Prompt 28] «Тихий» перезапуск (довге утримання POWER, див. input_cfg::kPowerRestartHoldMs)
// ---------------------------------------------------------------------------
// Неблокуюча пауза між моментом, коли пін підсилювача став LOW, і ESP.restart().
constexpr uint32_t kRestartMuteMs = 300;

// Страховка: найдовше чекання піна підсилювача; після цього перезапуск у будь-якому разі
// (щоб завислий I2C не зробив неможливим вихід із завислого стану). Має перекривати
// amp_cfg::kAmpOffMuteWaitMs — це перевіряє static_assert в app_controller.cpp.
constexpr uint32_t kRestartMaxWaitMs = 3000;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kTaskCore == 0, "AppController runs on core 0 (MASTER SPEC, section 5)");
static_assert(kTaskPriority < input_cfg::kBtnTaskPriority &&
                  kTaskPriority < input_cfg::kEncTaskPriority &&
                  kTaskPriority < ir_cfg::kTaskPriority,
              "AppController priority must be below input tasks (4)");
static_assert(kTaskPriority > display_cfg::kTaskPriority,
              "AppController priority must be above UI");
static_assert(kTaskPriority < player_cfg::kTaskPriority,
              "AppController priority must be below audio");
static_assert(kActivePollMs >= 1 && kIdlePollMs >= kActivePollMs,
              "poll periods: 1 <= active <= idle");
static_assert(kRampStep >= 1, "kRampStep must be >= 1");
static_assert(kRampStepMs >= 1, "kRampStepMs must be >= 1");
static_assert(kSyncPeriodMs >= kActivePollMs, "sync period too short");
static_assert(kAdjustTimeoutMs >= 1, "kAdjustTimeoutMs must be >= 1");
static_assert(kVolumeStep >= 1, "kVolumeStep must be >= 1");
static_assert(kRestartMuteMs >= 1 && kRestartMaxWaitMs > kRestartMuteMs,
              "restart timings: 1 <= mute pause < max wait");
static_assert((kInputUpStep == 1 || kInputUpStep == -1) &&
                  (kInputDownStep == 1 || kInputDownStep == -1) &&
                  (kStationLeftStep == 1 || kStationLeftStep == -1) &&
                  (kStationRightStep == 1 || kStationRightStep == -1),
              "direction steps must be +1 or -1");

}  // namespace app_controller_cfg
