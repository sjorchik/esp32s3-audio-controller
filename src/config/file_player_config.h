#pragma once

// [USB7] Константи відтворення файлу з USB-флешки (AudioPlayer::playFile, src/audio/audio_player.*).
// Піни й I2S тут не дублюються. Часові значення — у мілісекундах.
// Позначки: [ГАДКА] — вибір планування БЕЗ вимірювання, потребує підтвердження на залізі (hwcheck_USB7.md).

#include <stddef.h>
#include <stdint.h>

#include "config/features.h"
#include "config/station_level_config.h"
#if FEATURE_USB_HOST
#include "config/usb_host_config.h"
#endif

namespace file_player_cfg {

// Макс. довжина шляху до файлу разом з '\0' (шлях БЕЗ точки монтування). Має бути >= usb_host_cfg::kPathMax.
constexpr size_t kPathMax = 256;

// Рівень виходу декодера для файлу, дБ (output_trim). ПРИПУЩЕННЯ: той самий типовий рівень станції, щоб не було
// стрибка гучності відносно радіо. Не виміряно: перше прослуховування — на низькій гучності. [ГАДКА]
constexpr int kFileLevelDb = station_level_cfg::kDefaultStationLevelDb;

// Скільки чекати переходу Buffering -> Playing після connecttoFS, поки не прийшла жодна подія декодера
// (bitrate / "SampleRate (Hz)" / "stream ready"). Запасний вихід «файл грає»: isRunning() і минув цей час. [ГАДКА]
constexpr uint32_t kAssumePlayingMs = 1500;

// Скільки максимум чекати старту (Buffering) перед відмовою «start timeout». [ГАДКА]
constexpr uint32_t kStartTimeoutMs = 5000;

// isRunning() == false довше за цей час без події eof = збій читання / декодера (не reconnect!). [ГАДКА]
constexpr uint32_t kNotRunningGraceMs = 1500;

// Після події eof: пауза перед мʼютом і «файл закінчився» — щоб хвіст у I2S DMA дограв. [ГАДКА]
constexpr uint32_t kEndTailMs = 300;

// Період оновлення кешу позиції / тривалості (виконує задача плеєра; бібліотека не потокобезпечна). [ГАДКА]
constexpr uint32_t kPosPollMs = 200;

// Пауза між розмʼютом ЦАП (XSMT HIGH) і цифровим розмʼютом radio_volume: soft-unmute PCM5102 має завершитись, поки
// цифровий сигнал ще тихий. Нарощування звуку далі робить DSP (dsp_volume_cfg::kRampMs). Додає до початку файлу
// фейд-ін ≈ ця пауза + kRampMs. [ГАДКА] Підбір за клацанням на старті музики (партія 3 hwcheck_USB7).
constexpr uint32_t kDigitalUnmuteDelayMs = 30;

// Додаткова пауза перед XSMT HIGH для файлу (поверх xsmt::clockSettleMs()): PLL ЦАП після перезапуску I2S0 може
// захоплювати такт довше, ніж після зміни частоти в радіо. Змінюється з консолі: `usb.fx extra <мс>`. [ГАДКА]
constexpr uint32_t kExtraSettleMs = 0;
constexpr uint32_t kTuneMaxMs = 2000;  // верхня межа для usb.fx

// Консоль usb.seek: скільки чекати після seek перед повторним читанням позиції. [ГАДКА]
constexpr uint32_t kSeekSettleMs = 1000;

// Назви задач для вимірювання стека (uxTaskGetStackHighWaterMark). Внутрішня задача декодера бібліотеки
// (за спекою розділ 11: `PeriodicTask`, ядро 1, prio 2) і задача плеєра (audio_player.cpp).
constexpr const char* kDecoderTaskName = "PeriodicTask";
constexpr const char* kPlayerTaskName = "audio_player";

static_assert(kFileLevelDb >= station_level_cfg::kLevelMinDb && kFileLevelDb <= station_level_cfg::kLevelMaxDb,
              "file level must be inside the station level range");
static_assert(kPathMax >= 64, "kPathMax too small");
static_assert(kStartTimeoutMs > kAssumePlayingMs, "start timeout must exceed the assume-playing delay");
#if FEATURE_USB_HOST
static_assert(kPathMax >= usb_host_cfg::kPathMax, "player path buffer smaller than the USB wrapper path limit");
#endif

}  // namespace file_player_cfg
