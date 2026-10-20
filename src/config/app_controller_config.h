#pragma once

// Константи модуля core/app_controller.
// Піни тут не дублюються (їх немає: AppController не чіпає залізо напряму).
// Усі часові значення — у мілісекундах, якщо не вказано інше.
//
// [Prompt 48a] ЗМІНЕНО: чип-селектор вирізано. ВИДАЛЕНО kNeutralBass / Treble / Balance / Gain / Loudness / Volume
// (і static_assert-и до драйверів), kMuteRetryMs (повтор I2C-мʼюту) та kExtInputSettleMs: пауза після
// перемикання реле тепер input_selector_cfg::kRelaySettleMs (config/input_selector_config.h). ДОДАНО
// kLegacyToneValue / kLegacyLoudness — значення застарілих полів AppStateData (до P50).
// [Prompt 47c] ДОДАНО: kExtMuteSettleMs, kExtInputSettleMs, kLineIdleWaitMs, kLineStartTimeoutMs,
// kLineErrCheckDelayMs, kLineRetryDelayMs, kLineReleaseTimeoutMs, kExtLevelTrimDb (зовнішні входи через line_path).
// [Prompt 47a] ДОДАНО: kNeutralVolume (фіксована гучність чіпа), kRadioMuteWaitMs (очікування
// згасання цифрового мʼюту радіо). Програмна рампа гучності прибрана (її робить audio/dsp_volume):
// kRampStepMs / kRampStep НЕ ВИКОРИСТОВУЮТЬСЯ (прибрати в P52).

#include <esp_system.h>  // [Prompt 39] esp_reset_reason_t
#include <stdint.h>

#include "config/audio_player_config.h"  // player_cfg::kTaskPriority, ir_cfg, input_cfg
#include "config/display_config.h"      // display_cfg::kTaskPriority
#include "config/dsp_volume_config.h"   // [Prompt 47a] dsp_volume_cfg::kMuteRampMs
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
// Переходи: мʼют -> зміна входу/станції -> розмʼют
// ---------------------------------------------------------------------------
// Пауза між командами зміни (вхід/станція/пробудження) і розмʼютом.
constexpr uint32_t kUnmuteDelayMs = 250;

// [Prompt 47a] Скільки максимум чекати (блокуюче, vTaskDelay(1) у задачі контролера), поки цифровий
// мʼют радіо дозгасне, ПЕРЕД зупинкою плеєра / зміною станції / вимкненням підсилювача. Чекання
// відбувається лише коли радіо реально грає. Має перекривати dsp_volume_cfg::kMuteRampMs (20 мс) із
// запасом на блок хука (≈ 5 мс); після таймауту йде примусовий мʼют (requestForceMute()).
constexpr uint32_t kRadioMuteWaitMs = 60;

// ---------------------------------------------------------------------------
// [Prompt 47c] Зовнішні входи 1..4 через audio/line_path
// ---------------------------------------------------------------------------
// Пауза після цифрового мʼюту проходу (LinePath::setUserMute(true)) перед зміною входу чіпа / returnToRadio().
// Має перекривати dsp_volume_cfg::kMuteRampMs (20 мс) із запасом на блок (≈2,7 мс) і період tick.
constexpr uint32_t kExtMuteSettleMs = 40;
// [Prompt 48a] Пауза після перемикання реле-селектора (сигнал на PCM1808 стабілізується) перед розмʼютом —
// input_selector_cfg::kRelaySettleMs (було kExtInputSettleMs = 30 мс). Далі ще 100 мс плавного наростання DspVolume.
// Скільки чекати AudioPlayer::state() == Idle (stop() асинхронний) перед LinePath::requestStart().
// Більше за line_path_cfg::kRadioStopWaitMs (2000).
constexpr uint32_t kLineIdleWaitMs = 3000;
// Від успішного requestStart() до state() == Running (типово ≈ 650..800 мс).
constexpr uint32_t kLineStartTimeoutMs = 5000;
// Відразу після requestStart() lastError() може бути застарілим: ознаку «старт перервано» беремо лише після цієї паузи.
constexpr uint32_t kLineErrCheckDelayMs = 100;
// Пауза перед єдиним автоповтором старту після невдачі.
constexpr uint32_t kLineRetryDelayMs = 1000;
// Скільки чекати звільнення ресурсів після returnToRadio() (resourcesUp() == false, state == Off).
constexpr uint32_t kLineReleaseTimeoutMs = 3000;
// Зсув рівня зовнішніх входів відносно кривої радіо, дБ (≤ 0). 0 = та сама крива radioVolumeDb(). Підгонка
// «на слух»: якщо зовнішні входи гучніші за радіо при однаковому UI — зробити від'ємним (напр. −8).
constexpr float    kExtLevelTrimDb = 0.0f;

// [Prompt 47a] НЕ ВИКОРИСТОВУЄТЬСЯ: ramp гучності тепер цифровий (audio/dsp_volume, kRampMs = 100 мс).
constexpr uint32_t kRampStepMs = 20;
constexpr int      kRampStep   = 2;   // у одиницях шкали гучності UI (0..100)

// [Prompt 31] Автоповернення цілі регулювання енкодера на гучність.
// [Prompt 46a] НЕ ВИКОРИСТОВУЄТЬСЯ: цілей бас/дискант/баланс/gain більше немає, ціль завжди Volume.
// Константа лишена, щоб не чіпати інші файли; прибрати в P52.
constexpr uint32_t kAdjustTimeoutMs = 5000;

// Скільки тримати мʼют атенюаторів після зміни gain (апаратний стрибок рівня).
// [Prompt 46a] НЕ ВИКОРИСТОВУЄТЬСЯ (gain більше не регулюється); прибрати в P52.
constexpr uint32_t kGainMuteHoldMs = 80;

// ---------------------------------------------------------------------------
// [Prompt 48a] Застарілі поля AppStateData
// ---------------------------------------------------------------------------
// Чип вирізано: тембр, gain і loudness більше не існують, баланс повернеться як цифровий (P48b, dB). Поля
// AppStateData bass / treble / balance / gain / loudness (структуру не міняємо до P50) публікуються цими
// НЕЙТРАЛЬНИМИ значеннями. Фіксовану нейтральну гучність чипа (kNeutralVolume) видалено: чипа немає.
constexpr int8_t kLegacyToneValue = 0;
constexpr bool   kLegacyLoudness  = false;

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

// true: після БУДЬ-ЯКОГО старту (також після перезапусків) лишатись у Standby (мʼют, потік не
// запускається); false: відновити останній вхід/станцію й увімкнутись з ramp. [Prompt 39] Холодний
// старт (див. нижче) іде в Standby незалежно від цього прапорця.
constexpr bool kBootInStandby = false;

// ---------------------------------------------------------------------------
// [Prompt 39] Холодний старт у standby
// ---------------------------------------------------------------------------
// true: після ВВІМКНЕННЯ ЖИВЛЕННЯ (причина скидання з kColdStartResetReasons) пристрій стартує у
// Standby так, ніби натиснули POWER: підсилювач вимкнено, мʼют, потік не запускається. Усі інші
// скидання (ESP.restart() = веб reboot / OTA / «тихий» перезапуск, паніка, watchdog, deep sleep...)
// поводяться як раніше: пристрій вмикається й відновлює lastInput/lastStation.
constexpr bool kStandbyOnColdStart = true;

// Причини скидання, що вважаються «холодним стартом». Власник може додати сюди, наприклад,
// ESP_RST_EXT або ESP_RST_USB, якщо його спосіб перезапуску повідомляє іншу причину.
// УВАГА: на ESP32-S3 апаратне скидання кнопкою EN / DTR-RTS при прошивці по UART зазвичай
// теж повідомляється як ESP_RST_POWERON (відрізнити його від подачі живлення неможливо), тож
// такі скидання теж підуть у standby. Фактичну причину друкує лог `[APP] ... reset reason N`.
constexpr esp_reset_reason_t kColdStartResetReasons[] = {
    ESP_RST_POWERON,
    ESP_RST_BROWNOUT,
};

inline bool isColdStartReason(esp_reset_reason_t reason) {
    for (const esp_reset_reason_t r : kColdStartResetReasons) {
        if (r == reason) {
            return true;
        }
    }
    return false;
}

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
static_assert(kRadioMuteWaitMs > dsp_volume_cfg::kMuteRampMs,
              "radio mute wait must exceed the digital mute ramp");
static_assert(kSyncPeriodMs >= kActivePollMs, "sync period too short");
static_assert(kExtMuteSettleMs > dsp_volume_cfg::kMuteRampMs, "ext mute settle must exceed the digital mute ramp");
static_assert(kExtLevelTrimDb <= 0.0f, "ext level trim must not boost");
static_assert(kLineStartTimeoutMs > kLineErrCheckDelayMs && kLineIdleWaitMs >= 1 && kLineReleaseTimeoutMs >= 1,
              "line timeouts");
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