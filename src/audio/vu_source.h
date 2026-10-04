#pragma once

// Абстракція «джерело рівня VU».
// Потрібна, щоб пізніше додати PCM1808 без переробки UI.
// У версії 1 VU рахується з декодованих PCM-семплів
// і працює лише в режимі радіо.
//
// [Prompt 17] Заглушку замінено. Позначки:
//   ДОДАНО — нове (інтерфейс VuSource не змінено).
//
// Потоки даних:
//   продюсер  — audio_player.cpp, callback перехоплення PCM (ядро 1, гарячий шлях):
//               рахує пік по каналах і викликає VuSourceDecodedPcm::publishPeaks();
//   споживач  — ui/screens (display-задача, ядро 0): VuSourceDecodedPcm::read().
// Обмін — без мʼютекса: lock-free atomics (див. vu_source.cpp).
// read() викликає ОДНА задача (display); стан згладжування — у екземплярі.

#include <stdint.h>

class VuSource {
public:
    virtual ~VuSource() = default;

    // Прочитати поточні рівні.
    // Повертає нормалізовані значення 0..1.
    // false — даних немає (потік не грає / дані протухли): left = right = 0.
    virtual bool read(float& left, float& right) = 0;
};

// Джерело з декодованих PCM-семплів радіо.
class VuSourceDecodedPcm : public VuSource {
public:
    // Споживач. Викликати лише з однієї задачі (display). Без блокувань.
    bool read(float& left, float& right) override;

    // ДОДАНО: продюсер. Викликається з аудіо-callback-а ядра 1: peakL/peakR — максимум
    // |семпла| у шкалі int16 за блок (0..32768), frames — кількість стереокадрів у блоці.
    // Без блокувань, без логування; при ENABLE_VU == 0 порожня.
    static void publishPeaks(uint32_t peakL, uint32_t peakR, uint32_t frames);

private:
    // Лише display-задача.
    bool     m_init = false;
    bool     m_flowing = false;     // нові блоки надходять (для логу)
    bool     m_everFlowed = false;  // з моменту старту прийшов хоча б один блок
    bool     m_warnedNoData = false;
    uint32_t m_lastReadMs = 0;
    uint32_t m_sinceMs = 0;         // початок поточного опитування (для попередження)
    uint32_t m_lastBlocks = 0;
    uint32_t m_lastChangeMs = 0;
    float    m_levelL = 0.0f;
    float    m_levelR = 0.0f;
};
