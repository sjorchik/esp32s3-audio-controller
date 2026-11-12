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
// 1 — модуль збирається й може стартувати (begin() / end(), див. USB_HOST_AUTOSTART); 0 — модуль,
// бібліотека lib/usb_host_msc і USB-host не потрапляють у збірку (RAM / Flash не змінюються).
// Рідний Type-C НЕ використовувати одночасно з гніздом USB-A (спільні D+ / D−).
#define FEATURE_USB_HOST 1

// [USB2] Діагностична консоль USB (Serial: usb.stat / usb.ls / usb.cycle / usb.heap / usb.begin;
// [USB3] + usb.end / usb.life N). Діє лише при FEATURE_USB_HOST = 1. За замовчуванням 1, бо потрібна для
// hwcheck_USB2.md / hwcheck_USB3.md (вимірювання RAM, цикли begin / end); у РЕЛІЗІ ставити 0 (при 0 увесь
// діагностичний код, DEBUG-рівні логів драйвера MSC й опитування Serial у loop() компілюються геть).
#define USB_HOST_DIAG 1

// [USB2] Автозапуск USB-host в кінці setup(). 1 — begin() викликається автоматично; 0 — USB не запускається,
// поки його не підніме команда `usb.begin` (USB_HOST_DIAG = 1) або майбутній режим «USB».
// [USB3] ЗМІНЕНО: за замовчуванням 0 (рішення власника, варіант 2: USB-host піднімається лише в режимі «USB»;
// USB-host без носія ≈ 18,8 КБ внутрішньої RAM). Без діагностики (USB_HOST_DIAG = 0) значення 0 означає, що USB
// не стартує взагалі, доки режим «USB» не додано в AppController (крок g).
#define USB_HOST_AUTOSTART 0

// [USB9] Режим «USB» (бекенд, крок g1): «диригент» життєвого циклу enter / exit USB-host і відтворення файлів
// (src/usb/usb_mode.*, константи — config/usb_mode_config.h, мінімальні виклики в core/app_controller.*).
// Керується лише діагностичною консоллю (usb.mode …, потребує USB_HOST_DIAG = 1); UI / RC5 / веб — крок g2.
// 1 — модуль збирається, AppController отримує режим «USB» (команди зміни входу під час режиму відхиляються);
// 0 — вся нова логіка не збирається, поведінка пристрою не змінюється. Потребує FEATURE_USB_HOST = 1.
#define FEATURE_USB_MODE 1
#if FEATURE_USB_MODE && !FEATURE_USB_HOST
#error "FEATURE_USB_MODE = 1 requires FEATURE_USB_HOST = 1 (config/features.h)"
#endif

// [DEF5a] Карта внутрішньої кучі для DEF-5 (src/usb/usb_heapmap.*, константи — config/usb_heapmap_config.h): знімки
// обходом heap_caps_walk(MALLOC_CAP_INTERNAL) на контрольних точках циклу «радіо -> USB -> радіо», діф блоків між точками
// й між циклами, хук невдалих виділень без друку з контексту хука, консоль `usb.hm …`. ЛИШЕ діагностика (нічого не
// виправляє). 0 — нуль коду й RAM, поведінка збірок не змінюється; 1 — потребує USB_HOST_DIAG = 1 і FEATURE_USB_MODE = 1.
// Під 1 обхід DEF-5 (ssl_workaround) НЕ перезапускає плату (лише лог «would restart»), щоб збій не стирав стан.
#define USB_HEAPMAP 0
#if USB_HEAPMAP && !USB_HOST_DIAG
#error "USB_HEAPMAP = 1 requires USB_HOST_DIAG = 1 (config/features.h)"
#endif
#if USB_HEAPMAP && !FEATURE_USB_MODE
#error "USB_HEAPMAP = 1 requires FEATURE_USB_MODE = 1 (config/features.h)"
#endif

// [DEF5b] Запобіжна перевірка перед https-підключенням радіо після USB-сесії (src/core/ssl_preflight.*, константи —
// config/ssl_preflight_config.h): два пробні виділення по 16 717 Б (буфери TLS) INTERNAL|8BIT; не вмістились (навіть після
// очікування ≤ 600 мс) — підключення НЕ починається, замовляється ОДИН тихий перезапуск у ту саму станцію. WORKAROUND для
// DEF-5 (причину не усуває). Діє лише при FEATURE_USB_HOST = 1 (без USB-сесій перевіряти нічого); 0 — перевірка, її
// консольна команда `usb.preflight` і виклик у AudioPlayer::connectNow() повністю прибрані (inline-заглушки). Під
// USB_HEAPMAP = 1 перевірка логується, але перезапуск НЕ робиться (діагностика DEF-5 бачить збій).
#define SSL_PREFLIGHT 1

// [DEF5d] Великі буфери mbedtls (>= tls_psram_cfg::kMinBytes = 16 384 Б, тобто два буфери запису TLS по 16 717 Б) — у PSRAM
// через mbedtls_platform_set_calloc_free() (src/core/tls_psram.*, константи — config/tls_psram_config.h); дрібні — як і досі в
// internal. Тоді DEF-5 (`(-32512)` після USB-сесії) не залежить від розкладки внутрішньої купи. tls_psram::install() — на
// початку setup(). 0 — install() порожній, виділення TLS як раніше (internal), проби ssl_preflight — лише internal.
// Не залежить від FEATURE_USB_HOST. Консоль `usb.tls [stat]` — лише при USB_HOST_DIAG = 1.
#define TLS_PSRAM 1

// [DEF6] Сторожовий таймер кінця локального файлу (src/audio/audio_player.cpp, константи — config/file_player_config.h):
// якщо позиція файлу в стані Playing не змінюється >= kEofStallMs і до кінця лишилось <= kEofNearEndSec, плеєр СИНТЕЗУЄ
// подію eof (зупиняє файл і піднімає той самий прапорець, що й справжній eof), щоб usb_queue перейшла на наступний трек.
// 1 — синтез увімкнено (виробнича поведінка); 0 — лише діагностика (при USB_HOST_DIAG = 1: лог + лічильник, без дій), щоб
// виміряти частоту завмирань. Діє лише для локальних файлів; радіо-потоки не чіпає.
#define FILE_EOF_WATCHDOG 1

