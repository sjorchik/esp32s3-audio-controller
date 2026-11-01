#pragma once

// [USB2] Константи модуля монтування USB-флешки (src/usb/usb_msc_mount.*).
// Прапорці умовної компіляції (FEATURE_USB_HOST, USB_HOST_DIAG, USB_HOST_AUTOSTART) — у config/features.h.
//
// Позначки джерел значень:
//   [USB1]  — значення з перевіреного на залізі тесту USB1 (src/usb_host_test.cpp);
//   [W1]    — випливає з правила проєкту про ядра / пріоритети (MASTER SPEC, розділ 5, урок W1);
//   [ГАДКА] — вибір планування БЕЗ вимірювання, ПОТРЕБУЄ ПІДТВЕРДЖЕННЯ на залізі (hwcheck_USB2.md).

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

// --- Діагностична консоль (USB_HOST_DIAG) ------------------------------------------------
constexpr uint32_t kConsolePollMs = 50;     // [ГАДКА] період опитування Serial у loop() (за відсутності xsmt-консолі)
constexpr size_t kConsoleLineMax = 64;      // [ГАДКА] довжина рядка команди
constexpr size_t kPathMax = 256;            // [USB1] (у тесті буфер шляху 192; тут запас для підкаталогів)
constexpr int kLsMaxLines = 50;             // [ГАДКА] ліміт рядків `usb.ls`
constexpr uint32_t kLsLockWaitMs = 1000;    // [ГАДКА] очікування мʼютекса вводу-виводу в `usb.ls`
constexpr uint32_t kCycleGapMs = 500;       // [ГАДКА] пауза між розмонтуванням і монтуванням у `usb.cycle`
constexpr uint32_t kCycleMaxCount = 100;    // [ГАДКА] верхня межа N для `usb.cycle N`

}  // namespace usb_host_cfg
