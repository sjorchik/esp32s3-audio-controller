#pragma once

// [DEF5b] Константи запобіжної перевірки перед https-підключенням радіо після USB-сесії (core/ssl_preflight.*).
// Окремий файл (а не usb_host_config.h): правило 4 спеки забороняє дописувати в наявні config-файли, а перевірка
// не залежить від внутрішніх констант USB-host. Усі невиміряні значення — ПРИПУЩЕННЯ (позначено).

#include <stddef.h>
#include <stdint.h>

namespace ssl_preflight_cfg {

// Розмір одного пробного виділення = РІВНО той запит, що зафіксував хук невдалих виділень у збоях DEF-5 2026-11-08
// (16 717 Б, caps INTERNAL|8BIT, буфер TLS). ГІПОТЕЗА (не перевірена тут): TLSF округлює його до блока 17 408 Б.
constexpr size_t kTlsBufBytes = 16717;

// Скільки буферів TLS потрібно одночасно (у справних підключеннях видно ДВА: вхідний і вихідний).
constexpr uint8_t kProbeCount = 2;

// Очікування, якщо перевірка не пройшла: повтор кожні kStepMs до kMaxWaitMs від першої спроби. ПРИПУЩЕННЯ: відкладені
// звільнення задач (−2 блоки / 4 720 Б у справних циклах) встигають за ≈ 30–100 мс. Блокує ЛИШЕ задачу плеєра (ядро 1,
// там само, де connecttohost() блокує на секунди), тому kMaxWaitMs тримати невеликим.
constexpr uint32_t kStepMs = 100;
constexpr uint32_t kMaxWaitMs = 600;

// Обмежувач частоти перезапусків за перевіркою: не раніше ніж через стільки мс аптайму (страховка від помилки в коді).
// ПРИПУЩЕННЯ. Структурно цикл неможливий і без нього: прапорець «була USB-сесія» живе в RAM і скидається перезапуском.
constexpr uint32_t kMinUptimeMs = 15000;

// Скільки мс після запиту перезапуску плеєр не пробує підключатись знову (перезапуск у нормі ≤ ≈ 5 с:
// kPrepareRestartWaitMs = 4000 + пауза). Лише запасна межа, якщо перезапуск чомусь затримається.
constexpr uint32_t kRetryHoldMs = 30000;

// Задача перезапуску (одноразова; prepareForRestart() блокує викликача, тому не з задачі плеєра).
// Значення — ті самі, що й у ssl_workaround (usb_host_cfg::kRestartTask*): стек 5 120 Б там виміряно достатнім.
constexpr uint32_t kRestartTaskStack = 5120;
constexpr unsigned kRestartTaskPriority = 2;
constexpr int kRestartTaskCore = 0;

static_assert(kTlsBufBytes >= 4096 && kTlsBufBytes <= 65536, "DEF5b: sane TLS buffer size");
static_assert(kProbeCount >= 1 && kProbeCount <= 4, "DEF5b: probe count 1..4");
static_assert(kStepMs >= 20 && kStepMs <= kMaxWaitMs, "DEF5b: step <= max wait");
static_assert(kMaxWaitMs <= 2000, "DEF5b: the wait blocks the player task");
static_assert(kRetryHoldMs >= 10000, "DEF5b: hold must outlast a quiet restart");

}  // namespace ssl_preflight_cfg
