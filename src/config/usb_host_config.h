#pragma once

// [USB2] Константи модуля монтування USB-флешки (src/usb/usb_msc_mount.*).
// [USB3] ДОДАНО константи end() / usb.life (блок «Життєвий цикл»). Решта значень — без змін.
// [USB6] ДОДАНО блок fs::FS-обгортки (наприкінці файлу). Решта значень — без змін.
// [USB13] ДОДАНО блок «Закриття дескрипторів при розмонтуванні й діагностика пам'яті» (наприкінці файлу).
// [USB7] ДОДАНО kFsSlowReadUs (лічильники читань у usb_fs, USB_HOST_DIAG). Решта значень — без змін.
// [USB4] ДОДАНО блок «Діагностика begin() / end()» (лише USB_HOST_DIAG). Решта значень і ВСІ дефолти
// пріоритетів / таймаутів — без змін.
// Прапорці умовної компіляції (FEATURE_USB_HOST, USB_HOST_DIAG, USB_HOST_AUTOSTART) — у config/features.h.
//
// Позначки джерел значень:
//   [USB1]  — значення з перевіреного на залізі тесту USB1 (src/usb_host_test.cpp);
//   [W1]    — випливає з правила проєкту про ядра / пріоритети (MASTER SPEC, розділ 5, урок W1);
//   [ГАДКА] — вибір планування БЕЗ вимірювання, ПОТРЕБУЄ ПІДТВЕРДЖЕННЯ на залізі (hwcheck_USB2.md / hwcheck_USB3.md).

#include <stddef.h>
#include <stdint.h>

namespace usb_host_cfg {

// Точка монтування VFS. [USB1]
constexpr const char* kMountPath = "/usb";

// --- Ядро й пріоритети задач -------------------------------------------------------------
// Усі три задачі (usb_lib, внутрішня задача драйвера MSC, usb_mgr) — на ядрі 0: на ядрі 1 лише
// аудіо (задача `line_path` prio 6, декодер бібліотеки prio 2, loopTask). [W1]
// Для usb_lib і usb_host_install: ISR USB (ESP_INTR_FLAG_LEVEL1) виділяється на ядрі, з якого
// викликано usb_host_install, тому інсталяція виконується всередині задачі usb_mgr на ядрі 0
// (у тесті USB1 її викликали з setup(), тобто з ядра 1). [ГАДКА]
constexpr int kTaskCore = 0;  // [W1]

// Пріоритети на ядрі 0: введення = 4, AppController = 3, UI = 2, AsyncTCP = 2, веб / мережа ≈ 1.
// Вимога кроку: «нижче за мережеві / UI» => 1. У тесті USB1 було 3 (на ядрі 0, без радіо й UI).
// РИЗИК: при prio 1 під навантаженням ядра 0 (Wi-Fi, UI) можливі затримки читання USB.
// Якщо `fread` покаже стрибки затримки — піднімати до 2 (рівень UI), крайній варіант — 3 як у USB1.
constexpr unsigned kLibTaskPriority = 1;  // [ГАДКА] потребує підтвердження вимірюванням
constexpr unsigned kMscTaskPriority = 1;  // [ГАДКА] потребує підтвердження вимірюванням
constexpr unsigned kMgrTaskPriority = 1;  // [ГАДКА] монтування не критичне за часом

// --- Стеки, байти (IDF: у байтах) --------------------------------------------------------
constexpr uint32_t kLibTaskStack = 4096;  // [USB1] (usb_lib)
constexpr uint32_t kMscTaskStack = 4096;  // [USB1] (фонова задача драйвера MSC)
constexpr uint32_t kMgrTaskStack = 6144;  // [ГАДКА] монтування FatFS + Serial.printf; у тесті
                                          // працювало на стеку loopTask (8 КБ). Дивись `usb.stat`
                                          // (мін. вільний стек) і зменшуй / збільшуй за результатом.

// --- Черга подій -------------------------------------------------------------------------
constexpr size_t kEventQueueLen = 8;  // [ГАДКА] USB1: 4; одна флешка, події рідкі; +команди консолі

// --- Монтування FatFS (esp_vfs_fat_mount_config_t) ---------------------------------------
// ФОРМАТУВАТИ НОСІЙ ЗАБОРОНЕНО. [USB1]
constexpr bool kFormatIfMountFailed = false;
static_assert(!kFormatIfMountFailed, "USB2: formatting the user's media is forbidden");
constexpr int kMaxFiles = 4;                 // [USB1] одночасно відкритих файлів на томі
constexpr size_t kAllocationUnitSize = 0;    // [USB1] 0 = значення за замовчуванням (важливе лише для форматування)

// Спроби монтування: при помилці (наприклад, одноразовий `Data phase error` на старій флешці в USB1)
// повторити. Детермінована помилка (як 0x1701 на 32 ГБ) від повторів не зникне — лише лог.
constexpr uint8_t kMountMaxAttempts = 2;       // [ГАДКА]
constexpr uint32_t kMountRetryDelayMs = 1000;  // [ГАДКА] пауза між спробами

// API драйвера не має таймаута монтування й не дозволяє перервати `msc_host_vfs_register`;
// тому це ЛИШЕ поріг попередження в логу «монтування повільне».
constexpr uint32_t kMountSlowWarnMs = 3000;  // [ГАДКА]

// begin() чекає завершення інсталяції USB-host (usb_mgr) не довше цього часу.
constexpr uint32_t kInstallWaitMs = 1500;  // [ГАДКА]

// --- [USB3] Життєвий цикл: end() ----------------------------------------------------------
// end() (виклик із будь-якої задачі) чекає завершення зняття в usb_mgr не довше цього часу.
// Запас на найгірший випадок «end() під час монтування»: усі спроби монтування
// (kMountMaxAttempts × kMountSlowWarnMs + паузи між ними) + кроки зняття нижче.
// Це поріг очікування, а не гарантія: при таймауті end() повертає false, зняття триває у фоні.
constexpr uint32_t kEndWaitMs = 10000;  // [ГАДКА] перевірити виміром (usb.end під час монтування)

// Очікування одного кроку зняття (у задачі usb_mgr): подія ALL_FREE після usb_host_device_free_all(),
// вихід задачі usb_lib, повтори usb_host_uninstall() при ESP_ERR_INVALID_STATE («є незавершені дії»,
// usb/usb_host.h). Джерело: у заголовках таймаутів немає. [ГАДКА]
constexpr uint32_t kEndStepWaitMs = 2000;

// Крок опитування подій бібліотеки з usb_mgr між повторами usb_host_uninstall() (usb_lib на цей час уже
// зупинено, тож usb_host_lib_handle_events() викликає лише usb_mgr — заголовок забороняє виклик з двох
// потоків одночасно). [ГАДКА]
constexpr uint32_t kEndPollMs = 50;

// --- [USB5] Безпечне зняття стеку ---------------------------------------------------------
// Тихе вікно usb_lib: end() не переходить до наступного етапу зняття, поки протягом цього часу usb_lib не мала
// жодної активності (повернення usb_host_lib_handle_events() з ESP_OK або тривалий виклик = обробка події,
// див. libBusyNow()). Використовується двічі: до msc_host_uninstall і після ALL_FREE.
// ПРИПУЩЕННЯ — уточнюється sweep'ом (`usb.sweep`, лог `endlog` і `lib_gap_max_ms`). Точка відліку: тривалість
// перелічення з USB4 (connected ≈ 413–506 мс, mounted ≈ 691–794 мс) — НЕ значення вікна; вікно має перекривати
// найбільшу паузу між подіями usb_lib під час перелічення (її друкує `lib_gap_max_ms`, поки невідома).
// Нижня межа обмежена коректністю виявлення «обробка триває»: ≥ kLibWaitMs + 2 × kEndPollMs (static_assert нижче).
// Ціна: end() триває щонайменше kEndQuietMs (без флешки) або ≈ 2 × kEndQuietMs (з флешкою) + кроки зняття.
constexpr uint32_t kEndQuietMs = 250;  // ПРИПУЩЕННЯ — уточнюється sweep'ом

// Кількість ПОВТОРІВ usb_host_uninstall() після ESP_ERR_INVALID_STATE («є незавершені дії»: ненульові прапорці
// подій у usb_host.c v5.5.2; цей шлях повертає помилку БЕЗ аборту). Перед кожною спробою — num_devices == 0.
// Між спробами usb_mgr обробляє події (kEndPollMs). [ГАДКА] 5 × 50 мс ≈ 250 мс — поки без вимірювання, скільки
// повторів потрібно; sweep покаже, чи повтори взагалі виникають (рядок `uninstall` у `endlog`).
constexpr uint32_t kEndUninstallRetries = 5;

// Таймаут usb_host_lib_handle_events() у задачі usb_lib (замість portMAX_DELAY): щоб задача бачила
// прапорець зупинки навіть якщо usb_host_lib_unblock() не «запамʼятовується» (заголовок цього не гарантує).
// ESP_ERR_TIMEOUT у цьому режимі — норма. [ГАДКА]
constexpr uint32_t kLibWaitMs = 100;

// Пауза в end() після успішного зняття: задача, що видалила себе (vTaskDelete), звільняє стек і TCB
// лише в idle-задачі; без паузи вимір free одразу після end() занижений. [ГАДКА]
constexpr uint32_t kEndSettleMs = 100;

// begin() / end() чекають мьютекс життєвого циклу не довше: найдовше утримання — end() (kEndWaitMs)
// або begin() (kInstallWaitMs) + запас.
constexpr uint32_t kLifeLockWaitMs = kEndWaitMs + kInstallWaitMs + 1000;

// --- Діагностична консоль (USB_HOST_DIAG) ------------------------------------------------
constexpr uint32_t kConsolePollMs = 50;     // [ГАДКА] період опитування Serial у loop() (за відсутності xsmt-консолі)
constexpr size_t kConsoleLineMax = 64;      // [ГАДКА] довжина рядка команди
constexpr size_t kPathMax = 256;            // [USB1] (у тесті буфер шляху 192; тут запас для підкаталогів)
constexpr int kLsMaxLines = 50;             // [ГАДКА] ліміт рядків `usb.ls`
constexpr uint32_t kLsLockWaitMs = 1000;    // [ГАДКА] очікування мʼютекса вводу-виводу в `usb.ls`
constexpr uint32_t kCycleGapMs = 500;       // [ГАДКА] пауза між розмонтуванням і монтуванням у `usb.cycle`
constexpr uint32_t kCycleMaxCount = 100;    // [ГАДКА] верхня межа N для `usb.cycle N` і `usb.life N`

// [USB3] usb.life: після begin() чекати появи носія (подія драйвера) не довше; без флешки цикл
// триває цей час щоразу.
constexpr uint32_t kLifePresentWaitMs = 1500;  // [ГАДКА]
// usb.life: очікування стану Mounted після появи носія = усі спроби монтування з паузами та порогом
// «повільного» монтування (виводиться з констант вище, а не вигадується).
constexpr uint32_t kLifeMountWaitMs =
    kMountMaxAttempts * (kMountSlowWarnMs + kMountRetryDelayMs);

// --- [USB4] Діагностика begin() / end() (використовується лише при USB_HOST_DIAG = 1) -----------
// Крок опитування usb_host_lib_info() у `usb.begin` (проба появи пристрою). [ГАДКА]
constexpr uint32_t kProbePollMs = 20;
// Крок друку стану під час очікування ALL_FREE в end(): сумарне очікування лишається kEndStepWaitMs,
// очікування лише ділиться на кроки, щоб бачити, чи змінюється num_devices. [ГАДКА]
constexpr uint32_t kEndDiagSliceMs = 250;

// [USB5] Інваріанти тихого вікна й очікувань зняття.
static_assert(kEndQuietMs >= kLibWaitMs + 2 * kEndPollMs,
              "USB5: quiet window must exceed a 'waiting' handle_events call plus two polls to detect a busy call");
static_assert(kEndStepWaitMs > kEndQuietMs, "USB5: quiet wait is bounded by kEndStepWaitMs, must be longer than the window");
static_assert(kEndUninstallRetries >= 1 && kEndUninstallRetries <= 20, "USB5: sane retry count");

// --- [USB6] fs::FS-обгортка над /usb (src/usb/usb_fs.*) ----------------------------------------
// Очікування блокування тому на одну операцію VFS (usb_msc_mount::volumeLock). Тримається лише на час
// однієї операції; unmount чекає його. Таймаут = том «зайнятий»: операція відмовляє, файл осиротіє. [ГАДКА]
constexpr uint32_t kFsLockWaitMs = 1000;
// Розмір блоку читання (як у USB2 / usb.fs.read). [USB2]
constexpr size_t kFsChunk = 4096;
// Блок selftest: CRC32 рахується на кожен блок 64 КБ. [ГАДКА]
constexpr size_t kFsSelftestBlock = 65536;
// selftest зберігає CRC перших стількох блоків (512 × 64 КБ = 32 МБ). [ГАДКА]
constexpr size_t kFsSelftestMaxBlocks = 512;
// Скільки випадкових seek з перевіркою CRC робить selftest (прохід 2). [ГАДКА]
constexpr uint32_t kFsSeekChecks = 32;
// Верхня межа N для usb.fs.multi (скільки файлів одночасно відкрити). [ГАДКА]
constexpr uint32_t kFsMultiMax = 8;

// --- [USB7] Лічильники читань usb_fs (лише USB_HOST_DIAG; usb.pstat) ---------------------------
// Читання обгортки (UsbFileImpl::read, разом з очікуванням блокування тому) довше за цей поріг, мкс,
// рахується як «повільне» (20 мс — поріг зі спеки / критеріїв USB6). [USB6]
constexpr uint32_t kFsSlowReadUs = 20000;

// --- [USB13] Закриття дескрипторів при розмонтуванні (DEF-9) і діагностика пам'яті (DEF-5) -----------------------------
// Скільки закриттів файлу може бути ВІДКЛАДЕНО, якщо блокування тому не взято за kFsLockWaitMs (див. usb_fs.cpp:
// UsbFileImpl::close). Відкладений дескриптор закривається при наступному закритті в тому самому монтуванні або в
// unmountLocked() ДО msc_host_vfs_unregister. Переповнення — запасний шлях: навмисний витік (як було до USB13) і
// лічильник leaked. [ГАДКА] 8: за вимірами USB12 одночасно відкрито 1–2 дескриптори.
constexpr size_t kFsDeferredMax = 8;
// Скільки невдалих виділень друкувати (esp_rom_printf у хуку) за одне завантаження; далі лише лічильники. [ГАДКА]
constexpr uint32_t kMemFailPrintMax = 24;

}  // namespace usb_host_cfg
