# ESP32-S3 Audio Controller

Internet radio + external analog inputs for ESP32-S3 (N16R8):
6 buttons, rotary encoder (PCNT), IR remote (RC5/RC5X, learning mode),
ST7789 320x170 color display (LovyanGFX), PCM5102 DAC (I2S),
TDA7318 / PT2313L audio processor (I2C), web interface + mDNS.

## Hardware

- MCU: ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM)
- DAC: PCM5102 (I2S), XSMT soft-mute
- Audio processor: TDA7318 or PT2313L (I2C 0x44, selected in NVS)
- Display: ST7789 170x320, horizontal 320x170, SPI2 (FSPI), LEDC backlight
- Controls: 6 buttons, encoder with button, VS1838B IR receiver
- Pin map: single source of truth in `src/config/pins.h`

## Build

Toolchain: VS Code + PlatformIO, platform pioarduino 55.03.37
(Arduino-ESP32 3.3.7 / IDF 5.5.2), board `esp32-s3-devkitc-1`,
flash 16 MB, octal PSRAM (`qio_opi`), console on UART0.

```bash
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial monitor, 115200
pio run -t buildfs      # build LittleFS image from data/
pio run -t uploadfs     # flash LittleFS image