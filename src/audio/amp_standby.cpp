// audio/amp_standby.cpp (Prompt 23c): вихідний пін standby підсилювача.

#include "config/features.h"

#if FEATURE_AMP_STANDBY

#include "audio/amp_standby.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "config/amp_config.h"
#include "config/pins.h"

#if AMP_DEBUG
#define AMP_LOG(...) Serial.printf("[AMP] " __VA_ARGS__)
#else
#define AMP_LOG(...) \
    do {             \
    } while (0)
#endif

namespace {

constexpr int kPin = pins::kAmpStby;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
// Заборонені піни ESP32-S3: 26..37 (flash/PSRAM), 19/20 (USB).
static_assert(kPin >= 0 && kPin <= 48, "amp standby pin out of GPIO range");
static_assert(!(kPin >= 26 && kPin <= 37) && kPin != 19 && kPin != 20,
              "amp standby pin is a forbidden ESP32-S3 pin (flash/PSRAM/USB)");
// Збігів з іншими пінами проєкту (pins.h) немає.
static_assert(kPin != pins::kSt7789Sclk && kPin != pins::kSt7789Mosi &&
                  kPin != pins::kSt7789Cs && kPin != pins::kSt7789Dc &&
                  kPin != pins::kSt7789Rst && kPin != pins::kSt7789Blk,
              "amp standby pin collides with ST7789");
static_assert(kPin != pins::kI2sBclk && kPin != pins::kI2sWs && kPin != pins::kI2sDout &&
                  kPin != pins::kI2sMclk && kPin != pins::kI2sDin && kPin != pins::kXsmt,
              "amp standby pin collides with I2S/XSMT");
static_assert(kPin != pins::kI2cSda && kPin != pins::kI2cScl && kPin != pins::kEncA &&
                  kPin != pins::kEncB && kPin != pins::kEncBtn,
              "amp standby pin collides with I2C/encoder");
static_assert(kPin != pins::kBtnPower && kPin != pins::kBtnUp && kPin != pins::kBtnDown &&
                  kPin != pins::kBtnLeft && kPin != pins::kBtnRight && kPin != pins::kBtnOk,
              "amp standby pin collides with buttons");
static_assert(kPin != pins::kIrIn && kPin != pins::kUart1Tx && kPin != pins::kUart1Rx &&
                  kPin != pins::kUart0Tx && kPin != pins::kUart0Rx,
              "amp standby pin collides with IR/UART");
// Чи здатен пін бути виходом — перевіряє сам SDK (макрос є в IDF 5.x; якщо його немає в
// конкретній збірці, перевірка тихо пропускається).
#ifdef GPIO_IS_VALID_OUTPUT_GPIO
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(kPin), "amp standby pin cannot be an output on this SoC");
#endif

volatile bool s_run = false;
bool s_ready = false;

inline uint8_t levelFor(bool run) {
    return (run == amp_cfg::kRunLevelHigh) ? HIGH : LOW;
}

}  // namespace

bool AmpStandby::begin() {
    s_run = false;
    // Защолкуємо рівень standby в регістрі виходу, ПОКИ пін ще вхід (після скидання — з
    // внутрішнім pulldown), потім вмикаємо драйвер: на піні немає короткого імпульсу.
    gpio_set_level(static_cast<gpio_num_t>(kPin), levelFor(false));
    pinMode(kPin, OUTPUT);
    digitalWrite(kPin, levelFor(false));
    s_ready = true;
    AMP_LOG("GPIO%d -> standby\n", kPin);
    return true;
}

void AmpStandby::set(bool run) {
    if (!s_ready && !begin()) {
        return;
    }
    if (run == s_run) {
        return;
    }
    s_run = run;
    digitalWrite(kPin, levelFor(run));
    AMP_LOG("%s\n", run ? "run" : "standby");
}

bool AmpStandby::isRunning() {
    return s_run;
}

#endif  // FEATURE_AMP_STANDBY
