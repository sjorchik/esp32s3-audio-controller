#pragma once

// [USB14] Константи входу «Плеєр» (поки лише джерело USB): профіль входу, відновлення при вході в режим, пауза файлу.
// Прапорець умовної компіляції FEATURE_USB_MODE — у config/features.h (тут не дублюється).
//
// Позначки джерел значень:
//   [USB6/USB10] — з виміряних кроків (розділ 11 MASTER SPEC);
//   [P47a]       — цифровий мʼют радіо (radio_volume) гасне ≈ 20 мс;
//   ПРИПУЩЕННЯ   — вибір БЕЗ вимірювання, підтверджується в hwcheck_USB14.md.

#include <stddef.h>
#include <stdint.h>

#include "config/defaults.h"

// --- Єдина точка «джерело плеєра» ---------------------------------------------------------------------------------------
// Поки існує лише USB. Майбутні microSD / Bluetooth додаються в кінець enum (числові значення можуть потрапити в NVS-ключі
// через імена джерел; для SD ключі будуть `pl_sd_*`). Реалізації SD / BT тут НЕМАЄ.
enum class PlayerSource : uint8_t {
    Usb = 0,
};
constexpr uint8_t kPlayerSourceCount = 1;

namespace player_mode_cfg {

// --- Профіль входу «Player» (4-й слот Settings::profiles) -----------------------------------------------------------------
// Слот стоїть ПІСЛЯ логічних входів (0 радіо, 1 TV Box, 2 Computer): defaults::kInputCount НЕ змінюється, тож усі цикли
// «по входах» (веб, коло входів, маска) лишаються по 3. Профіль «Плеєр» бачить лише AppController (режим «USB»).
constexpr uint8_t kProfileIndex = defaults::kInputCount;
constexpr uint8_t kProfileCount = defaults::kInputCount + 1;

// --- Відновлення при вході в режим -----------------------------------------------------------------------------------------
// true — після монтування стати в останню папку й почати відтворення останнього треку зі збереженої позиції; false —
// лише стати в папку й підготувати чергу (OK запускає трек зі збереженої позиції).
constexpr bool kAutoResumeOnEnter = true;
// Макс. довжина збереженого шляху папки / імені треку разом з '\0'. [USB6] = usb_host_cfg::kPathMax (звірено static_assert-ом
// у usb_queue.cpp).
constexpr size_t kResumePathMax = 256;
// Скільки чекати кореневого списку після першого Mounted, перш ніж відмовитись від відновлення. ПРИПУЩЕННЯ: читання кореня —
// типово одиниці–десятки мс [USB10], запас на повільну флешку.
constexpr uint32_t kResumeWaitRootMs = 15000;
// Скільки чекати завершення одного кроку (перехід у папку). ПРИПУЩЕННЯ: збігається з очікуванням консолі [USB10].
constexpr uint32_t kResumeStepTimeoutMs = 8000;
// Період оновлення RAM-знімка позиції в черзі (НЕ запис у NVS). Позиція в плеєрі оновлюється кожні kPosPollMs = 200 мс.
constexpr uint32_t kSnapshotPeriodMs = 1000;

// --- Пауза файлу (AudioPlayer::setFilePaused) ------------------------------------------------------------------------------
// Скільки чекати цифровому мʼюту radio_volume (плавно, ≈ 20 мс [P47a]) ПЕРЕД XSMT LOW на паузі. ПРИПУЩЕННЯ: 30 мс.
constexpr uint32_t kPauseFadeMs = 30;

// --- Ключі NVS (namespace «audioctl», спільний з Settings і IrRc5; ≤ 15 символів) -----------------------------------------
// Відокремлені за джерелом: для SD з'являться `pl_sd_*`.
constexpr const char* kNvsKeyUsbDir = "pl_usb_dir";
constexpr const char* kNvsKeyUsbTrack = "pl_usb_trk";
constexpr const char* kNvsKeyUsbPos = "pl_usb_pos";

static_assert(kProfileCount == defaults::kInputCount + 1, "Player profile follows the logical inputs");
static_assert(kResumePathMax >= 64, "resume path buffer too small");

}  // namespace player_mode_cfg
