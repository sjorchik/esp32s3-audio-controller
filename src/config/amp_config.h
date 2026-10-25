#pragma once

// Константи модуля audio/amp_standby та логіки standby підсилювача в AppController.
// Сам пін — pins::kAmpStby (GPIO46). Усі часові значення — у мілісекундах.
// Прапорець вимкнення всієї можливості — FEATURE_AMP_STANDBY у config/features.h.

#include <stdint.h>

// Serial-лог переходів піна: "[AMP] run" / "[AMP] standby".
#define AMP_DEBUG 1

namespace amp_cfg {

// Рівень на піні, що означає «підсилювач працює». true: HIGH = працює, LOW = standby.
constexpr bool kRunLevelHigh = true;

// Прогрів після підняття піна: скільки Settle (під мʼютом) чекає, перш ніж AppController
// розмʼютить атенюатори. Типова вихідна ступінь виходить зі standby за 100..500 мс
// (залежить від конденсатора на виводі STBY/MUTE підсилювача). 400 мс — запас із середини
// діапазону; точне значення підібрати на слух (клацання при вмиканні -> збільшити).
// Прогрів рахується від моменту підняття піна, паралельно з kUnmuteDelayMs (250 мс):
// розмʼют настане через max(kUnmuteDelayMs, kAmpWakeMs).
constexpr uint32_t kAmpWakeMs = 400;

// Пауза між мʼютом атенюаторів і пониженням піна при вході в standby: щоб сигнал на вході
// підсилювача вже стих (I2C-запис мʼюту + розряд розділових конденсаторів + XSMT ЦАП
// при зупинці потоку). Задача не блокується: це таймер у tick().
constexpr uint32_t kAmpOffDelayMs = 200;

// Якщо мʼют атенюаторів так і не підтверджено (I2C не відповідає), пін усе одно
// опускається не пізніше цього часу від входу в standby (пристрій має вимкнутись).
// Більше за kMuteRetryMs (500), щоб встигли хоча б дві повторні спроби мʼюту.
constexpr uint32_t kAmpOffMuteWaitMs = 1500;

static_assert(kAmpWakeMs <= 5000, "kAmpWakeMs too long: device would feel unresponsive");
static_assert(kAmpOffDelayMs <= kAmpOffMuteWaitMs,
              "kAmpOffDelayMs must not exceed kAmpOffMuteWaitMs");

}  // namespace amp_cfg
