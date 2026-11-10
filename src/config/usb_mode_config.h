#pragma once

// [USB9] Константи режиму «USB» (src/usb/usb_mode.*): життєвий цикл enter / exit, очікування, ліміти консолі.
// Прапорець умовної компіляції FEATURE_USB_MODE — у config/features.h.
//
// Позначки джерел значень:
//   [USB3..5] — значення з виміряних кроків USB3 – USB5 (розділ 11 MASTER SPEC; посилання на константи usb_host_cfg);
//   [W1]      — правило проєкту про ядра / пріоритети (розділ 5, урок W1);
//   ПРИПУЩЕННЯ — вибір планування БЕЗ вимірювання, підтверджується / уточнюється в hwcheck_USB9.md (таблиця T);
//   [ГАДКА]   — те саме, але другорядне (крок опитування, кількість повторів).

#include <stddef.h>
#include <stdint.h>

#include "config/usb_host_config.h"

namespace usb_mode_cfg {

// --- Фонова задача (одноразова: створюється на enter / exit і сама видаляється) ----------------------------------
// Чому не постійна задача: головний аргумент власника за ліниве піднімання USB — внутрішня RAM (radio https:
// free ≈ 40 КБ до begin). Постійний стек зайняв би її весь час; одноразова задача тримає стек лише на 0,3 – 10 с.
// Чому не AppController: end() блокує 0,6 – 1,25 с (до kEndWaitMs), begin() до kInstallWaitMs — це заблокувало б
// керування. Чому не usb_mgr: end() з задачі usb_mgr заборонено (usb_msc_mount.h).
constexpr int kTaskCore = 0;                    // [W1] ядро 0: на ядрі 1 лише аудіо
constexpr unsigned kTaskPriority = 1;           // [W1] «не вище мережевих»: wifi 1, async_tcp 2, usb_* 1
constexpr uint32_t kTaskStack = 4096;           // ПРИПУЩЕННЯ: Serial.printf + виклики end() / begin(); мін. вільний стек друкує usb.mode stat
constexpr const char* kTaskName = "usb_mode";

// --- Опитування --------------------------------------------------------------------------------------------------
constexpr uint32_t kPollMs = usb_host_cfg::kProbePollMs;  // [USB4] 20 мс — крок проби появи пристрою

// --- Вхід ---------------------------------------------------------------------------------------------------------
// Скільки чекати, поки радіо / прохід замовкнуть і звільнять RAM (AudioPlayer Idle, line_path без ресурсів, I2S0 не
// зафіксовано на 48 кГц). ПРИПУЩЕННЯ: найгірший випадок — звільнення проходу (AppController: kExtMuteSettleMs 40 мс +
// returnToRadio, до kLineReleaseTimeoutMs = 3000 мс) плюс зупинка плеєра, що може стояти в блокуючому connecttohost().
// При таймауті вхід відкочується (AppController::usbModeLeave), стан Off з причиною «quiet_timeout».
constexpr uint32_t kQuietWaitMs = 5000;
// Пауза після «тиші» перед виміром RAM і begin(): задачі, що видалили себе, звільняють стек / TCB лише в idle-задачі.
constexpr uint32_t kRamSettleMs = usb_host_cfg::kEndSettleMs;  // [USB3] 100 мс — та сама причина

// Очікування носія після begin(): USB4 — перелічення в radio: enum_seen 341–402, connected 413–506, mounted ≤ 794 мс.
// Фаза 1 (пристрій ще не з'явився): kEnterPresenceWaitMs. Якщо за цей час стан не змінився з NoDevice — флешки нема
// (Ready без носія; монтування почнеться само при вставленні — host лишається піднятим).
constexpr uint32_t kEnterPresenceWaitMs = usb_host_cfg::kLifePresentWaitMs;  // [USB3] 1500 мс ≈ 2× найбільший mounted (794)
// Фаза 2 (пристрій з'явився, іде монтування): усі спроби монтування разом із паузами й порогом «повільного».
constexpr uint32_t kEnterMountWaitMs = usb_host_cfg::kLifeMountWaitMs;       // [USB3] kMountMaxAttempts × (3000 + 1000)

// --- Вихід --------------------------------------------------------------------------------------------------------
// Скільки чекати Idle плеєра після AudioPlayer::stop() (мʼют ЦАП із паузою kMuteLeadMs + stopSong). ПРИПУЩЕННЯ.
constexpr uint32_t kFileStopWaitMs = 2000;
// Повтори виклику AppController::usbModeEnter / usbModeLeave, якщо мʼютекс контролера зайнятий (kLockTimeoutMs = 200).
constexpr uint8_t kAppCallRetries = 5;           // [ГАДКА]
constexpr uint32_t kAppCallRetryMs = 100;        // [ГАДКА]

// --- Діагностична консоль (USB_HOST_DIAG) -------------------------------------------------------------------------
constexpr uint32_t kCycleMaxCount = usb_host_cfg::kCycleMaxCount;  // [USB3] верхня межа N для usb.mode cycle
// Верхні межі очікувань у usb.mode cycle (виводяться з констант вище, а не вигадуються).
constexpr uint32_t kCycleEnterWaitMs =
    kQuietWaitMs + kRamSettleMs + usb_host_cfg::kInstallWaitMs + kEnterPresenceWaitMs + kEnterMountWaitMs + 2000;
constexpr uint32_t kCycleExitWaitMs =
    kFileStopWaitMs + usb_host_cfg::kEndWaitMs + kRamSettleMs + (kAppCallRetries * kAppCallRetryMs) + 2000;
// Після exit цикл чекає, поки відновлений вхід реально зазвучить (радіо Playing / прохід Running), щоб RAM вимірювалась
// у тому самому стані, що й базовий вимір. ПРИПУЩЕННЯ: TLS-підключення станції + наповнення буфера ≤ 15 с.
constexpr uint32_t kRestoreSettleMs = 15000;

// --- [USB11] Навігація: коло входів (Радіо -> TV Box -> Computer -> USB), дія INPUT_USB, відкладена ціль входу -------
// Логіка — core/app_controller.cpp (блок «USB11»); значення тут. Усе нижче — ПРИПУЩЕННЯ планування без вимірювання,
// підтверджується в hwcheck_USB11.md.
// Скільки повторювати usb_mode::requestEnter() щотакту, якщо він відмовив «busy» (попередня операція ще завершується
// після Off: задача usb_mode знімає s_busy до стану, але сама ще видаляється). Далі ціль скидається з логом.
constexpr uint32_t kNavEnterBusyRetryMs = 1000;
// Страховка від зависання відкладеної цілі: найгірший вхід + найгірший вихід (виведено з уже наявних меж) + запас.
// Типово вхід ≈ 1 с, вихід ≈ 1–2 с (USB9); перевищення означає збій автомата, а не повільний носій.
// (kCycleEnterWaitMs / kCycleExitWaitMs оголошено вище.)
constexpr uint32_t kNavPendingTimeoutMs = kCycleEnterWaitMs + kCycleExitWaitMs + 5000;
// Стан Error із піднятим прапорцем режиму (end() не вдався / вхід не відкотився): «пристрій не лишається залиплим».
// Пауза між появою Error і дією — щоб діагностичні рядки usb_mode / usb_msc_mount встигли вийти в Serial.
constexpr uint32_t kNavErrorGraceMs = 300;
// Скільки разів повторити usb_mode::requestExit() у Error, якщо стек справний (needsReboot() == false; end_not_quiet).
// Після вичерпання — перезапуск. [ГАДКА]
constexpr uint8_t kNavExitRetries = 2;
// true: Error із піднятим прапорцем режиму -> «тихий» перезапуск (AppController::beginRestart: мʼют -> підсилювач
// STBY -> пауза -> ESP.restart()), із чітким логом причини. false: пристрій лишається в режимі «USB» без звуку
// (поведінка USB9), вихід — лише POWER-утримання / живлення. Режим «USB» НЕ відновлюється після перезапуску.
constexpr bool kNavRestartOnError = true;

static_assert(kEnterPresenceWaitMs >= 1 && kEnterMountWaitMs > kEnterPresenceWaitMs, "usb_mode: wait windows");
static_assert(kNavEnterBusyRetryMs >= 1 && kNavExitRetries <= 10, "usb_mode: nav limits");
static_assert(kCycleMaxCount >= 1 && kCycleMaxCount <= 1000, "usb_mode: sane cycle limit");
static_assert(kTaskPriority <= 2, "usb_mode task must not outrank the network tasks (async_tcp = 2)");

}  // namespace usb_mode_cfg
