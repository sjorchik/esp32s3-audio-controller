#pragma once

// Прототип проходу зовнішнього звуку (Prompt 44): PCM1808 (I2S1 RX slave) -> ESP32-S3 ->
// PCM5102 (I2S1 TX slave) на тактах, які веде I2S0 бібліотеки ESP32-audioI2S.
// Без DSP: лише фіксоване послаблення з плавним наростанням/спадом. Старт — завжди в мʼюті.
//
// Весь модуль працює в одній задачі (ядро line_path_cfg::kTaskCore); публічні виклики лише
// ставлять запит і повертаються (крім геттерів). Компілюється при LINE_PATH_ENABLE == 1.
//
// Стани: Off -> Starting (чекаємо стабільні блоки) -> Running -> Stalled (нема блоків ≥ kStallMs:
// XSMT low, підсилення 0; сам відновлюється) -> ... -> Stopping -> Off.
//
// Ділянка GPIO18 (DOUT): start() переводить її на I2S1 TX; stop() її НЕ повертає (лінія тиха,
// I2S1 TX вимкнено); returnToRadio() повертає GPIO18 на I2S0 і знімає фіксацію 48 кГц.

#include <stdint.h>

enum class LinePathState : uint8_t { Off, Starting, Running, Stalled, Stopping };

struct LinePathStats {
    LinePathState state;
    bool          muted;            // XSMT низько
    bool          routedToLine;     // GPIO18 зараз на I2S1 TX
    int8_t        attenuationDb;    // задане послаблення
    uint32_t      blocks;           // оброблено блоків (усього)
    uint32_t      readTimeouts;     // таймаути читання RX
    uint32_t      readErrors;       // інші помилки читання RX
    uint32_t      writeTimeouts;    // таймаути / помилки запису TX
    uint32_t      rxOverflows;      // переповнення черги RX DMA (ISR-callback)
    uint32_t      txOverflows;      // переповнення черги TX DMA (ISR-callback)
    uint32_t      stalls;           // скільки разів входили в Stalled
    uint32_t      starts;           // успішних стартів
    uint32_t      maxProcessUs;     // макс. час обробки + запису блоку (без очікування читання)
    uint32_t      maxReadWaitUs;    // макс. очікування читання блоку
    uint32_t      initMs;           // тривалість begin()
    uint32_t      lastStartMs;      // тривалість останнього start(): від запиту до XSMT high
    float         peakLeftDbfs;     // макс. пік входу АЦП (до послаблення), дБFS, з останнього resetStats(); -99 = тиша
    float         peakRightDbfs;
};

class LinePath {
public:
    // Створює I2S1 (duplex slave, канали вимкнені, GPIO18 НЕ чіпає) і задачу. Викликати ПІСЛЯ
    // AudioPlayer::begin(). Повторний виклик безпечний. false — помилка ініціалізації.
    static bool begin();
    static bool isReady();

    // Запит на старт: зупиняє радіо, фіксує 48 кГц на I2S0, переводить GPIO18 на I2S1, вмикає
    // канали й під мʼютом чекає стабільні блоки. true — запит прийнято.
    static bool start();

    // Запит на зупинку проходу: спад до 0, XSMT low, I2S1 вимкнено. GPIO18 лишається на I2S1.
    static bool stop();

    // Запит на повернення до радіо: (зупиняє прохід, якщо йде), GPIO18 -> I2S0, 48 кГц знято.
    // XSMT лишається низько (радіо саме зніме мʼют, коли потік стабільний).
    static bool returnToRadio();

    static bool isRunning();  // Starting / Running / Stalled
    static LinePathState state();
    static const char* stateName(LinePathState s);

    // Послаблення, дБ; обрізається до [kAttMinDb, kAttMaxDb]. Лише послаблення, наживо, плавно.
    static void setAttenuationDb(int8_t db);
    static int8_t attenuationDb();

    static LinePathStats stats();
    static void resetStats();
};
