#include "audio/audio_i2c.h"

#include <Arduino.h>
#include <Wire.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config/audio_config.h"
#include "config/defaults.h"
#include "config/pins.h"

namespace audio_i2c {

namespace {

SemaphoreHandle_t s_mutex = nullptr;
bool s_ready = false;

// Обмеження частоти логу помилок.
bool s_errLogged = false;
uint32_t s_lastErrLogMs = 0;

void logWriteError(uint8_t err, size_t len) {
    const uint32_t now = millis();
    if (s_errLogged && (now - s_lastErrLogMs) < audio_cfg::kI2cErrLogMinIntervalMs) {
        return;
    }
    s_errLogged = true;
    s_lastErrLogMs = now;
    // err: 1 = завеликий буфер, 2 = NACK на адресу, 3 = NACK на дані, 4 = інше, 5 = таймаут.
    Serial.printf("[AUDIO] I2C write failed: err=%u len=%u\n",
                  static_cast<unsigned>(err), static_cast<unsigned>(len));
}

}  // namespace

Lock::Lock() : m_taken(false) {
    if (s_mutex != nullptr) {
        m_taken = xSemaphoreTakeRecursive(
                      s_mutex, pdMS_TO_TICKS(audio_cfg::kI2cMutexTimeoutMs)) == pdTRUE;
    }
}

Lock::~Lock() {
    if (m_taken) {
        xSemaphoreGiveRecursive(s_mutex);
    }
}

bool begin() {
    if (s_ready) {
        return true;
    }
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_mutex == nullptr) {
            return false;
        }
    }
    // Підтяжки SDA/SCL зовнішні (до 3.3 В), внутрішні не вмикаємо.
    if (!Wire.begin(pins::kI2cSda, pins::kI2cScl, defaults::kI2cClockHz)) {
        return false;
    }
    Wire.setTimeOut(audio_cfg::kI2cTimeoutMs);
    s_ready = true;
    return true;
}

bool isReady() {
    return s_ready;
}

bool probe() {
    if (!s_ready) {
        return false;
    }
    Lock lock;
    if (!lock.ok()) {
        return false;
    }
    Wire.beginTransmission(defaults::kAudioProcessorI2cAddr);
    return Wire.endTransmission(true) == 0;
}

bool write(const uint8_t* data, size_t len) {
    if (!s_ready || data == nullptr || len == 0) {
        return false;
    }
    Lock lock;
    if (!lock.ok()) {
        return false;
    }
    uint8_t err = 4;
    for (uint8_t attempt = 0; attempt <= audio_cfg::kI2cRetries; ++attempt) {
        Wire.beginTransmission(defaults::kAudioProcessorI2cAddr);
        Wire.write(data, len);
        err = Wire.endTransmission(true);
        if (err == 0) {
            return true;
        }
    }
    logWriteError(err, len);
    return false;
}

}  // namespace audio_i2c
