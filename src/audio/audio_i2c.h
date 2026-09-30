#pragma once

// Спільна I2C-шина аудіопроцесорів (TDA7318 / PT2313L).
// Один статичний рекурсивний мʼютекс на обидва драйвери: якщо їх колись
// створять по черзі, вони не заважатимуть один одному, а шину не смикатимуть
// одночасно AppController і тестовий режим.
//
// Драйвери беруть Lock на весь метод (стан + кеш + запис), а write()/probe()
// беруть його ще раз (рекурсивно) — тому без взаємного блокування.

#include <stddef.h>
#include <stdint.h>

namespace audio_i2c {

// Створює мʼютекс і ініціалізує Wire (піни/швидкість/таймаут з config/).
// Ідемпотентна. Викликати з setup(), не з кількох задач одночасно.
bool begin();

bool isReady();

// Порожня транзакція на адресу процесора: true, якщо ACK. Стан не змінює.
bool probe();

// Одна транзакція запису (адреса + len байтів даних) з повторами.
// true — усі байти підтверджено. Помилку логує з обмеженням частоти.
bool write(const uint8_t* data, size_t len);

// RAII-захоплення мʼютекса шини (рекурсивне). ok() == false — не вдалося
// захопити за kI2cMutexTimeoutMs (або begin() ще не викликано).
class Lock {
public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    bool ok() const { return m_taken; }

private:
    bool m_taken;
};

// Кеш останніх байтів, ПІДТВЕРДЖЕНИХ чипом, по одному на регістр (до 8).
// Запис пропускається, якщо ті самі байти вже лежать у чипі. Після невдалого
// запису запис кешу скидається, тож наступний виклик з тим самим значенням
// таки піде в шину.
class RegShadow {
public:
    static constexpr uint8_t kMaxRegs = 8;

    void invalidateAll() { m_valid = 0; }

    void invalidate(uint8_t idx) {
        m_valid = static_cast<uint8_t>(m_valid & ~(1u << idx));
    }

    bool isSame(uint8_t idx, uint8_t value) const {
        return ((m_valid >> idx) & 1u) != 0 && m_value[idx] == value;
    }

    void set(uint8_t idx, uint8_t value) {
        m_value[idx] = value;
        m_valid = static_cast<uint8_t>(m_valid | (1u << idx));
    }

private:
    uint8_t m_value[kMaxRegs] = {};
    uint8_t m_valid = 0;
};

}  // namespace audio_i2c
