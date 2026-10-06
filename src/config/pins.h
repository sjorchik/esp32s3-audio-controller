#pragma once

// Єдина мапа пінів проекту.
// Джерело: MASTER_SPEC, розділ 4.
// Заборонені піни ESP32-S3: GPIO 26..37 (flash/PSRAM), 19/20 (USB).
// Не змінювати без прямої вказівки.

#include <stdint.h>

namespace pins {

// --- ST7789 (SPI2 / FSPI) ---
constexpr int kSt7789Sclk = 12;
constexpr int kSt7789Mosi = 11;
constexpr int kSt7789Cs   = 10;
constexpr int kSt7789Dc   = 9;
constexpr int kSt7789Rst  = 13;
constexpr int kSt7789Blk  = 14;   // LEDC PWM підсвітки

// --- I2S (PCM5102) ---
constexpr int kI2sBclk = 15;
constexpr int kI2sWs   = 17;      // LRCLK
constexpr int kI2sDout = 18;      // ESP32 -> DIN PCM5102
constexpr int kI2sMclk = 16;      // резерв PCM1808, НЕ драйвити
constexpr int kI2sDin  = 8;       // резерв PCM1808, не використовується

// --- Софт-мʼют PCM5102 ---
constexpr int kXsmt = 45;         // active-low, зовнішній pulldown 10 кОм

// --- Standby підсилювача (вихід) ---
constexpr int kAmpStby = 46;      // 1 = працює, 0 = standby; strapping-пін (pulldown 10 кОм до GND)

// --- I2C (TDA7318 / PT2313L) ---
constexpr int kI2cSda = 1;
constexpr int kI2cScl = 2;

// --- Енкодер ---
constexpr int kEncA   = 4;        // PCNT
constexpr int kEncB   = 5;        // PCNT
constexpr int kEncBtn = 6;

// --- Кнопки ---
constexpr int kBtnPower = 7;
constexpr int kBtnUp    = 21;
constexpr int kBtnDown  = 38;
constexpr int kBtnLeft  = 39;
constexpr int kBtnRight = 40;
constexpr int kBtnOk    = 41;

// --- IR приймач ---
constexpr int kIrIn = 47;         // RMT RX, active-low

// --- Резерв UART1 (майбутній BT-модуль) ---
constexpr int kUart1Tx = 42;
constexpr int kUart1Rx = 48;

// --- Консоль UART0 ---
constexpr int kUart0Tx = 43;
constexpr int kUart0Rx = 44;

}  // namespace pins