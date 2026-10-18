#pragma once

// Прохід зовнішнього звуку (Prompt 44, [P44b] — життєвий цикл ресурсів; P45 — DSP-ланцюжок):
// PCM1808 (I2S1 RX slave) -> ESP32-S3 -> PCM5102 (I2S1 TX slave) на тактах, які веде I2S0 бібліотеки
// ESP32-audioI2S. Старт — завжди в мʼюті.
//
// [P45] Ланцюжок блоку: RX -> піки входу -> EQ (eq::Processor, власний екземпляр) -> dsp_volume
// (цифрова гучність і мʼют) -> піки виходу -> TX. Гучність радіо лишається в TDA7318 до P47: dsp_volume
// для радіо не вмикається.
//
// Весь прохід працює в одній задачі (ядро line_path_cfg::kTaskCore); публічні виклики лише ставлять
// запит і повертаються (крім геттерів і створення ресурсів у start(), див. нижче).
// Компілюється при LINE_PATH_ENABLE == 1.
//
// [P44b] ЖИТТЄВИЙ ЦИКЛ РЕСУРСІВ (внутрішня RAM, ≈20 КБ): begin() НЕ створює каналів I2S1, задачі й
// буферів. Їх створює start() (з порожнього стану) і знищує returnToRadio(); stop() їх лишає (GPIO18
// залишається на I2S1, радіо німе). Під радіо line_path не тримає нічого кореневого, окрім стану.
//
// Стани: Off -> Starting (чекаємо стабільні блоки) -> Running -> Stalled (нема блоків ≥ kStallMs:
// XSMT low, підсилення 0; сам відновлюється) -> ... -> Stopping -> Off.
//
// Ділянка GPIO18 (DOUT): start() переводить її на I2S1 TX; stop() її НЕ повертає (лінія тиха,
// I2S1 TX вимкнено); returnToRadio() повертає GPIO18 на I2S0, знімає фіксацію 48 кГц і звільняє
// ресурси.

#include <stdint.h>

enum class LinePathState : uint8_t { Off, Starting, Running, Stalled, Stopping };

struct LinePathStats {
    LinePathState state;
    bool          muted;             // XSMT низько (і цифровий мʼют dsp_volume)
    bool          routedToLine;      // GPIO18 зараз на I2S1 TX
    int8_t        attenuationDb;     // задане послаблення (через dsp_volume)
    uint32_t      blocks;            // оброблено блоків (усього)
    uint32_t      readTimeouts;      // таймаути читання RX
    uint32_t      readErrors;        // інші помилки читання RX
    uint32_t      writeTimeouts;     // таймаути / помилки запису TX
    uint32_t      rxOverflows;       // переповнення черги RX DMA (ISR-callback)
    uint32_t      txOverflows;       // переповнення черги TX DMA (ISR-callback)
    uint32_t      stalls;            // скільки разів входили в Stalled
    uint32_t      starts;            // успішних стартів (відновлення після Stalled сюди НЕ входять)
    uint32_t      maxProcessUs;      // макс. час обробки + запису блоку (без очікування читання)
    uint32_t      maxReadWaitUs;     // макс. очікування читання блоку
    uint32_t      initMs;            // тривалість begin()
    uint32_t      lastStartMs;       // тривалість останнього start(): від запиту до XSMT high
    float         peakLeftDbfs;      // [P45] пік ВХОДУ (до EQ і гучності), дБFS, з resetStats(); -99 = тиша
    float         peakRightDbfs;
    float         peakOutLeftDbfs;   // [P45] пік ВИХОДУ (після dsp_volume), дБFS, з resetStats(); -99 = тиша
    float         peakOutRightDbfs;
    uint32_t      allocFailures;     // [P44b] start() відхилено через нестачу памʼяті / невдале створення каналів
    uint32_t      clipCount;         // [P45] насичені відліки на виході EQ, з resetStats()
};

class LinePath {
public:
    // [P44b] Лише готує стан (і Serial-тест під LINE_PATH_TEST); каналів, задачі й буферів НЕ створює,
    // GPIO18 НЕ чіпає. Викликати ПІСЛЯ AudioPlayer::begin(). Повторний виклик безпечний.
    // При LINE_PATH_ENABLE == 0 стартує лише Serial-тест (lp.heap) і повертає false.
    static bool begin();
    static bool isReady();

    // Запит на старт: XSMT low, створює ресурси (якщо їх нема), зупиняє радіо, фіксує 48 кГц на I2S0,
    // переводить GPIO18 на I2S1, вмикає канали й під мʼютом чекає стабільні блоки.
    // [P44b] Якщо ресурсів нема, створення (канали I2S1, буфер, задача) виконується ТУТ, і виклик
    // блокується на ≈ 10–20 мс (до kCreateWaitMs). false — запит відхилено: зайнято, не готово або
    // нестача памʼяті (тоді лічильник allocFailures росте, XSMT low, GPIO18 лишається на I2S0).
    // Невдача ПІСЛЯ повернення true (радіо не зупинилось, не застосувався 48 кГц, помилка enable) —
    // автоматичний відкат: GPIO18 -> I2S0, ресурси звільнено, state() лишається Off (див. лог).
    static bool start();

    // Запит на зупинку проходу: спад до 0 (dsp_volume, kMuteRampMs), XSMT low, I2S1 вимкнено.
    // GPIO18 лишається на I2S1, ресурси лишаються. false, якщо ресурсів нема (нічого зупиняти).
    static bool stop();

    // Запит на повернення до радіо: (зупиняє прохід, якщо йде), GPIO18 -> I2S0, 48 кГц знято,
    // канали / буфер / задача line_path знищуються. XSMT лишається низько (радіо саме зніме мʼют,
    // коли потік стабільний). Якщо ресурсів уже нема — лише гарантує, що фіксацію 48 кГц знято.
    static bool returnToRadio();

    static bool isRunning();  // Starting / Running / Stalled
    static LinePathState state();
    static const char* stateName(LinePathState s);

    // Послаблення, дБ; обрізається до [kAttMinDb, kAttMaxDb]. Лише послаблення, наживо, плавно
    // (через dsp_volume; перехід починається в наступному блоці).
    static void setAttenuationDb(int8_t db);
    static int8_t attenuationDb();

    static LinePathStats stats();
    static void resetStats();
};