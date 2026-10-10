#pragma once

// Прапорці можливостей.
// Використовуються для умовної компіляції та заглушок.

// Майбутній Bluetooth-модуль через UART1.
// У версії 1 не використовується.
#define ENABLE_BT_UART 0

// VU-метр: загальний вимикач. 0 — VU немає ні на радіо, ні на зовнішніх входах
// (блок VU на екрані порожній, джерело VuSourceDecodedPcm вимкнено).
// Радіо: рівні з декодованих PCM-семплів (VuSourceDecodedPcm).
// Входи 1..3: рівні з АЦП PCM1808 — окремо вмикаються ADC_VU_ENABLE
// (config/adc_vu_config.h), який потребує ENABLE_VU.
#define ENABLE_VU 1

// Обидва аудіопроцесори компілюються.
// Вибір конкретного зберігається в NVS і змінюється у веб-інтерфейсі.
// Автовизначення неможливе, бо адреса фіксована 0x44 і зчитування немає.
#define ENABLE_TDA7318 1
#define ENABLE_PT2313L 1

// Веб-інтерфейс і mDNS.
#define ENABLE_WEB 1
#define ENABLE_MDNS 1

// Керування standby підсилювача виходом pins::kAmpStby (1 = працює, 0 = standby).
// 0 — пін не чіпається, логіка затримок в AppController вимкнена.
#define FEATURE_AMP_STANDBY 1