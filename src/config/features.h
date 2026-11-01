#pragma once

// Прапорці можливостей.
// Використовуються для умовної компіляції та заглушок.

// Майбутній Bluetooth-модуль через UART1.
// У версії 1 не використовується.
#define ENABLE_BT_UART 0

// VU-метр: загальний вимикач. 0 — VU немає ні на радіо, ні на зовнішніх входах
// (блок VU на екрані порожній, джерело VuSourceDecodedPcm вимкнено).
// Радіо: рівні з декодованих PCM-семплів (VuSourceDecodedPcm).
// Зовнішні входи: рівні з проходу line_path (PCM1808 -> DSP -> PCM5102, VuSourceLinePath);
// потребують LINE_PATH_ENABLE (config/line_path_config.h) і ENABLE_VU.
#define ENABLE_VU 1

// [Prompt 48a] Прапорців ENABLE_TDA7318 / ENABLE_PT2313L більше немає: аудіочіпи й I2C вирізано,
// входи 1 / 2 комутує реле (audio/input_selector, пін pins::kInputRelay).

// Веб-інтерфейс і mDNS.
#define ENABLE_WEB 1
#define ENABLE_MDNS 1

// Керування standby підсилювача виходом pins::kAmpStby (1 = працює, 0 = standby).
// 0 — пін не чіпається, логіка затримок в AppController вимкнена.
#define FEATURE_AMP_STANDBY 1

// [USB2] USB-host (рідний порт S3, GPIO19 = D−, GPIO20 = D+) з драйвером MSC: монтування
// USB-флешки в /usb (модуль src/usb/usb_msc_mount.*, константи — config/usb_host_config.h).
// 1 — модуль збирається й стартує (за USB_HOST_AUTOSTART); 0 — модуль, бібліотека lib/usb_host_msc
// і USB-host не потрапляють у збірку (RAM / Flash не змінюються).
// Значення за замовчуванням 1 лише для кроку USB2 (власник перевіряє на залізі, hwcheck_USB2.md).
// Рідний Type-C НЕ використовувати одночасно з гніздом USB-A (спільні D+ / D−).
#define FEATURE_USB_HOST 1

// [USB2] Діагностична консоль USB (Serial: usb.stat / usb.ls / usb.cycle / usb.heap / usb.begin).
// Діє лише при FEATURE_USB_HOST = 1. За замовчуванням 1, бо потрібна для hwcheck_USB2.md
// (вимірювання RAM, цикли монтування); у РЕЛІЗІ ставити 0 (при 0 увесь діагностичний код,
// DEBUG-рівні логів драйвера MSC й опитування Serial у loop() компілюються геть).
#define USB_HOST_DIAG 1

// [USB2] Автозапуск USB-host в кінці setup(). 1 — begin() викликається автоматично (поведінка
// для цього кроку); 0 — USB не запускається, поки не прийде консольна команда `usb.begin`
// (потрібно для вимірювання RAM «до begin()», лише при USB_HOST_DIAG = 1; без діагностики
// 0 означає, що USB не стартує взагалі).
#define USB_HOST_AUTOSTART 1
