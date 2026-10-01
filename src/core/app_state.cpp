// Реалізація AppState: одна статична копія стану під мʼютексом FreeRTOS.

#include "core/app_state.h"

#include <string.h>

#include "config/defaults.h"

namespace {

AppStateData s_state;  // статична памʼять → нулі до begin()
SemaphoreHandle_t s_mutex = nullptr;

// RAII-захоплення. До begin() (мʼютекса немає) працює без блокування.
// Очікування необмежене: fn у modify() за контрактом коротка, а snapshot()
// не має способу повернути помилку.
class Lock {
public:
    Lock() {
        if (s_mutex != nullptr) {
            held_ = (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE);
        }
    }
    ~Lock() {
        if (held_) {
            xSemaphoreGive(s_mutex);
        }
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    bool held_ = false;
};

}  // namespace

bool AppState::begin() {
    if (s_mutex != nullptr) {
        return true;  // уже ініціалізовано, стан не скидаємо
    }
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (m == nullptr) {
        return false;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.mode = Mode::Standby;
    s_state.inputIndex = defaults::kDefaultInput;
    s_state.volume = defaults::kDefaultVolume;
    s_state.bass = defaults::kDefaultBass;
    s_state.treble = defaults::kDefaultTreble;
    s_state.balance = defaults::kDefaultBalance;
    s_state.mute = defaults::kDefaultMute;

    s_mutex = m;  // публікуємо мʼютекс після заповнення стану
    return true;
}

AppStateData AppState::snapshot() {
    Lock lock;
    AppStateData copy = s_state;
    return copy;
}

void AppState::update(const AppStateData& data) {
    Lock lock;
    s_state = data;
}

void AppState::modify(void (*fn)(AppStateData&)) {
    if (fn == nullptr) {
        return;
    }
    Lock lock;
    fn(s_state);
}

void AppState::modify(void (*fn)(AppStateData&, void*), void* ctx) {
    if (fn == nullptr) {
        return;
    }
    Lock lock;
    fn(s_state, ctx);
}
