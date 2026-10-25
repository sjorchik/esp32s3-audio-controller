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
