#pragma once

// [USB18a] ДОДАНО kSeekEndMarginSec (seek за кінець файлу = кінець треку) і kTagMaxBytes (ID3-теги для веб-API). Решта без змін.
// [DEF6] ДОДАНО kTailWatchSec / kTailLogMs / kTailLogMaxLines (монітор «хвоста», USB_HOST_DIAG), kEofStallMs / kEofNearEndSec
// (сторожовий таймер FILE_EOF_WATCHDOG), kEofStatNameMax (usb.eof). Решта без змін.
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

// [DEF6] Монітор «хвоста» (лише USB_HOST_DIAG; лише лог). Коли до кінця файлу лишилось <= kTailWatchSec секунд
// (getAudioFileDuration - getAudioCurrentTime, роздільність 1 с), раз на kTailLogMs пишеться рядок «[PLAYER] tail: ...»,
// не більше kTailLogMaxLines рядків на трек. [ПРИПУЩЕННЯ] 2 с / 500 мс / 12 рядків (≈ 6 с спостереження).
constexpr uint32_t kTailWatchSec = 2;
constexpr uint32_t kTailLogMs = 500;
constexpr uint8_t kTailLogMaxLines = 12;

// [DEF6] Сторожовий таймер кінця файлу (FILE_EOF_WATCHDOG): позиція файлу в стані Playing не змінюється >= kEofStallMs
// І до кінця лишилось <= kEofNearEndSec секунд (dur > 0) -> синтезується eof. Позиція має роздільність 1 с, тож
// kEofStallMs має суттєво перевищувати 1 с. [ПРИПУЩЕННЯ] 2500 мс / 3 с; підбір — за hwcheck_DEF6.md.
constexpr uint32_t kEofStallMs = 2500;
constexpr uint32_t kEofNearEndSec = 3;

// [DEF6] Довжина буфера імені файлу в лічильниках usb.eof (останній випадок; лише USB_HOST_DIAG).
constexpr size_t kEofStatNameMax = 96;

// [USB18a] Seek за кінець файлу: якщо (позиція + зсув) >= тривалість - kSeekEndMarginSec (dur > 0, зсув > 0), бібліотечний seek НЕ
// викликається, а трек завершується як звичайний кінець (синтез eof, як у DEF6). Запас покриває роздільність 1 с кешу
// позиції / тривалості. ПРИПУЩЕННЯ: 1 с.
constexpr uint32_t kSeekEndMarginSec = 1;
// [USB18a] Макс. байтів одного ID3-тегу (title / artist / album) разом з '\0', UTF-8; довші усікаються по межі символу.
// ПРИПУЩЕННЯ: 64 Б (≈ 30 кириличних літер).
constexpr size_t kTagMaxBytes = 64;
// Назви задач для вимірювання стека (uxTaskGetStackHighWaterMark). Внутрішня задача декодера бібліотеки
// (за спекою розділ 11: `PeriodicTask`, ядро 1, prio 2) і задача плеєра (audio_player.cpp).
constexpr const char* kDecoderTaskName = "PeriodicTask";
constexpr const char* kPlayerTaskName = "audio_player";

static_assert(kFileLevelDb >= station_level_cfg::kLevelMinDb && kFileLevelDb <= station_level_cfg::kLevelMaxDb,
              "file level must be inside the station level range");
static_assert(kPathMax >= 64, "kPathMax too small");
static_assert(kSeekEndMarginSec <= 10, "kSeekEndMarginSec: sane margin");
static_assert(kTagMaxBytes >= 8 && kTagMaxBytes <= 256, "kTagMaxBytes: sane tag size");
static_assert(kEofStallMs > 1000 + kPosPollMs, "eof stall time must exceed the 1 s position resolution");
static_assert(kTailLogMs > 0 && kTailLogMaxLines > 0 && kEofNearEndSec > 0 && kTailWatchSec > 0,
              "DEF6 tail / watchdog constants must be positive");
static_assert(kEofStatNameMax >= 16, "kEofStatNameMax too small");
static_assert(kStartTimeoutMs > kAssumePlayingMs, "start timeout must exceed the assume-playing delay");
#if FEATURE_USB_HOST
static_assert(kPathMax >= usb_host_cfg::kPathMax, "player path buffer smaller than the USB wrapper path limit");
#endif

}  // namespace file_player_cfg
