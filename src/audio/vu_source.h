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
//
// [VU1] Режим вимірювання (config/vu_config.h, VU_MEASURE_MODE): 0 = пік за блок (publishPeaks, як було),
// 1 = RMS (publishEnergy: сума квадратів + кількість кадрів; споживач усереднює за інтервал між read()).
// Інтерфейс read() не змінено, publishPeaks() збережено (у режимі 1 не викликається).

#include <stdint.h>

// [VU1] Стан споживача RMS (ЛИШЕ display-задача, лише VU_MEASURE_MODE == 1); логіка — vu_source.cpp.
// Лічильники base* — опубліковані продюсером кумулятивні значення (за модулем 2^32) на початок поточного
// вікна усереднення; msq* — середній квадрат останнього завершеного вікна (одиниці int16²).
struct VuRmsState {
    bool     init = false;
    bool     flowing = false;
    bool     everFlowed = false;
    bool     warnedNoData = false;
    uint32_t lastReadMs = 0;
    uint32_t sinceMs = 0;
    uint32_t lastBlocks = 0;
    uint32_t lastChangeMs = 0;
    uint32_t windowStartMs = 0;
    uint32_t baseL = 0;
    uint32_t baseR = 0;
    uint32_t baseFrames = 0;
    float    msqL = 0.0f;
    float    msqR = 0.0f;
    float    levelL = 0.0f;
    float    levelR = 0.0f;
};

// [V1] Чутливість VU по входах: статичні VuSource::setSensitivityDb() / sensitivityDb() (див. нижче), застосовується в
// read() обох джерел через vu_math::peakToNorm(peak, sensDb). Решту інтерфейсу не змінено.
class VuSource {
public:
    virtual ~VuSource() = default;

    // Прочитати поточні рівні.
    // Повертає нормалізовані значення 0..1.
    // false — даних немає (потік не грає / дані протухли): left = right = 0.
    virtual bool read(float& left, float& right) = 0;

    // [V1] ДОДАНО (інтерфейс read() не змінено): чутливість VU АКТИВНОГО входу, дБ (vu_sens_cfg::kMinDb..kMaxDb;
    // поза діапазоном обрізається). ОДНЕ значення на обидва джерела (радіо й прохід зовнішніх входів): рівень
    // читає лише джерело активного входу. Ставить AppController (ядро 0) при кожному застосуванні профілю входу
    // й при зміні значення; читають read() обох джерел (display-задача, ядро 0) — std::atomic<int8_t>, relaxed.
    // Аудіо-ядро (publishPeaks / хук) цього значення не бачить і не чіпає: зсув додається до рівня в дБ у
    // споживача, тож на звук і гарячий шлях не впливає. 0 = поведінка як до V1.
    static void   setSensitivityDb(int8_t db);
    static int8_t sensitivityDb();
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

    // [VU1] ДОДАНО: продюсер RMS (VU_MEASURE_MODE == 1). Викликається з аудіо-callback-а ядра 1 один раз на блок:
    // sumSqL / sumSqR — сума квадратів семплів блоку в шкалі int16 (повна шкала 32768 -> 2^30 на кадр),
    // frames — кількість стереокадрів у блоці. Лише float, без блокувань, логів, double і malloc;
    // вимір суми квадратів робить викликач. Єдиний письменник — задача продюсера. При ENABLE_VU == 0 порожня.
    static void publishEnergy(float sumSqL, float sumSqR, uint32_t frames);

private:
    VuRmsState m_rms;               // [VU1] лише display-задача
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

// [Prompt 47d] Джерело з проходу line_path (зовнішні входи). Замінило VuSourceAdc із adc_vu (модуль і шим
// видалено в P48d). Ті самі одиниці, що й у радіо:
// продюсер line_path публікує піки в шкалі int16 (0..32768) ПІСЛЯ еквалайзера й ДО гучності, з
// калібруванням line_path_cfg::kVuOffsetDb. Обмін — lock-free, як у VuSourceDecodedPcm.
// При LINE_PATH_ENABLE == 0 або ENABLE_VU == 0 read() завжди false, publishPeaks() порожня.
class VuSourceLinePath : public VuSource {
public:
    // Споживач. Викликати лише з однієї задачі (display). Без блокувань.
    bool read(float& left, float& right) override;

    // Продюсер: лише задача line_path (ядро 1). peakL/peakR — максимум |семпла| у шкалі int16
    // (з урахуванням калібрування), frames — кількість стереокадрів у блоці.
    static void publishPeaks(uint32_t peakL, uint32_t peakR, uint32_t frames);

    // [VU1] ДОДАНО: продюсер RMS (VU_MEASURE_MODE == 1), лише задача line_path (ядро 1), раз на блок.
    // sumSqL / sumSqR — сума квадратів у шкалі int16 З урахуванням калібрування kVuOffsetDb (після EQ, ДО гучності),
    // frames — кількість стереокадрів у блоці. Решта правил — як у VuSourceDecodedPcm::publishEnergy().
    static void publishEnergy(float sumSqL, float sumSqR, uint32_t frames);

private:
    VuRmsState m_rms;               // [VU1] лише display-задача
    // Лише display-задача.
    bool     m_init = false;
    bool     m_flowing = false;
    bool     m_everFlowed = false;
    bool     m_warnedNoData = false;
    uint32_t m_lastReadMs = 0;
    uint32_t m_sinceMs = 0;
    uint32_t m_lastBlocks = 0;
    uint32_t m_lastChangeMs = 0;
    float    m_levelL = 0.0f;
    float    m_levelR = 0.0f;
};

