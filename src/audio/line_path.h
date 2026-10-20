#pragma once

// Прохід зовнішнього звуку (Prompt 44; P44b — життєвий цикл ресурсів; P45 — DSP-ланцюжок;
// P47b — виробничий API). Ланцюжок: PCM1808 (I2S1 RX slave) -> ESP32-S3 -> PCM5102 (I2S1 TX slave)
// на тактах I2S0 бібліотеки ESP32-audioI2S.
//
// ЛАНЦЮЖОК БЛОКУ (P45, P47d): RX int32 -> float -> піки входу -> eq::Processor (власний екземпляр,
// processFloat) -> піки VU (після EQ, ДО гучності) -> DspVolume (processFloat) -> ОДНЕ насичення й
// перетворення в int32 -> піки виходу -> TX. VU зовнішніх входів публікується у VuSourceLinePath
// (audio/vu_source.h; шкала int16, калібрування line_path_cfg::kVuOffsetDb). Гучність радіо лишається в TDA7318 до P47; DspVolume для радіо не вмикається.
//
// ЖИТТЄВИЙ ЦИКЛ РЕСУРСІВ (P44b): begin() нічого не виділяє. start() / requestStart() створюють
// буфер блоку, задачу line_path і канали I2S1 (задача). returnToRadio() знищує їх; stop() ресурси лишає.
//
// [Prompt 48b] ДОДАНО: setBalanceDb() / balanceDb() — цифровий баланс зовнішніх входів (DspVolume проходу).
// ГУЧНІСТЬ, МʼЮТ, EQ (P47b):
//  - Гучність проходу: setVolumeDb(float) у [line_path_cfg::kVolMinDb, kVolMaxDb] = [−120, −6] дБ.
//    Верх −6 дБ — жорсткий максимум. Значення діє і до старту; до першого задання — kStartVolumeDb (−60).
//  - Два незалежні мʼюти: внутрішній (state != Running: старт, стоп, Stall, перехід) і мʼют користувача
//    setUserMute(). Цифровий мʼют = userMute || внутрішній. Внутрішні переходи мʼют користувача НЕ
//    скасовують, і навпаки. ТИПОВО мʼют користувача УВІМКНЕНИЙ: прохід після старту НЕ розмʼючується сам,
//    лише після setUserMute(false). XSMT (PCM5102) від мʼюту користувача НЕ залежить: слідує за станом
//    (Running = відкритий), тож при мʼюті користувача на ЦАП ідуть нулі (тиша).
//  - EQ входу: setEqAllDb / setEqBandDb / setEqBypass діють на власний екземпляр eq::Processor проходу.
//    Пресет існує від завантаження (плаский за замовчуванням), тож діє і до старту. Значення мовчки
//    обрізаються до eq_cfg (−12..+12 дБ), неіснуюча смуга ігнорується.
//
// ПОТОКИ:
//  - Публічні виклики — з будь-якої задачі: AppController (ядро 0), веб-обробники, Serial-тест.
//  - Обробка блоку, I2S1, XSMT (крім команди мʼюту з викликача) і лічильники RX/TX — лише задача line_path
//    (ядро 1). Поля, що пишуться ззовні, або атомарні, або змінюються в критичній секції (мʼюти).
//  - Команди старту / стопу / повернення сериалізуються неблокуючим try-lock (s_opLock). Якщо зайнято —
//    виклик повертає false, нічого не чекаючи.
//
// БЛОКУВАННЯ (верхні межі):
//  - start(): блокує викликача до line_path_cfg::kCreateWaitMs (500 мс) лише на створення каналів I2S1.
//    Типово 10–20 мс (за коментарем коду, окремо не вимірювалось). Повний старт (глушіння радіо,
//    дренаж, фіксація 48 кГц) іде в задачі; за P44b ≈ 650 мс.
//  - requestStart(): не блокує. true = запит прийнято; результат — state(), lastError(), stats().allocFailures.
//    Для AppController (P47c) рекомендовано саме його.
//  - stop(), returnToRadio(): не блокують (лише ставлять запит задачі).
//
// XSMT (власник — line_path): muteDac(true) у старті, стопі, поверненні, Stall; muteDac(false) при
// переході Starting -> Running. Викликач (start / requestStart) лише тримає XSMT низько до відповіді задачі.
//
// Компілюється при LINE_PATH_ENABLE == 1. При 0 усі методи — заглушки (нові символи збірку не ламають).

#include <stdint.h>

#include "config/eq_config.h"

enum class LinePathState : uint8_t { Off, Starting, Running, Stalled, Stopping };

// Причина останньої невдачі старту або аварії. Скидається на початку кожної спроби старту.
enum class LinePathError : uint8_t {
    None,                 // помилок немає
    AllocFailed,          // нестача внутрішньої RAM (буфер блоку)
    TaskCreateFailed,     // xTaskCreatePinnedToCore не вдалося
    ChannelCreateFailed,  // створення / ініціалізація каналів I2S1 у задачі
    CreateTimeout,        // start() не дочекався створення каналів (kCreateWaitMs)
    RadioStopTimeout,     // радіо не дійшло до Idle
    RadioRefused,         // плеєр відхилив фіксацію 48 кГц (не Idle)
    ClockFixFailed,       // 48 кГц не застосовано на I2S0 за kClockApplyWaitMs
    EnableFailed,         // i2s_channel_enable повернув помилку
    Stalled,              // аварія: блоки RX не надходять (kStallMs); самовідновлюється
};

struct LinePathStats {
    LinePathState state;
    bool          muted;             // XSMT низько (сам пін, не мʼют користувача)
    bool          routedToLine;      // GPIO18 зараз на I2S1 TX
    int8_t        attenuationDb;     // [P47b] volumeDb, округлено до цілого (для сумісності)
    uint32_t      blocks;
    uint32_t      readTimeouts;
    uint32_t      readErrors;
    uint32_t      writeTimeouts;
    uint32_t      rxOverflows;
    uint32_t      txOverflows;
    uint32_t      stalls;
    uint32_t      starts;
    uint32_t      maxProcessUs;
    uint32_t      maxReadWaitUs;
    uint32_t      initMs;
    uint32_t      lastStartMs;
    float         peakLeftDbfs;      // пік ВХОДУ (до EQ і гучності), дБFS; -99 = тиша
    float         peakRightDbfs;
    float         peakOutLeftDbfs;   // пік ВИХОДУ (після гучності), дБFS; -99 = тиша
    float         peakOutRightDbfs;
    uint32_t      allocFailures;     // відхилені старти (нестача памʼяті, таймаут, створення задачі)
    uint32_t      clipCount;         // [P47d] насичені відліки на виході проходу (ПІСЛЯ гучності; EQ не насичує)
    // [P47b] нові поля в кінці
    float         volumeDb;          // гучність проходу, дБ [−120..−6]
    bool          userMuted;         // мʼют користувача (setUserMute)
    LinePathError lastError;         // остання причина невдачі старту / аварії
};

class LinePath {
public:
    // Готує стан, мʼютекс логу, гучність (kStartVolumeDb) і мʼют. Каналів / задачі / буферів НЕ створює,
    // GPIO18 НЕ чіпає. Викликати ПІСЛЯ AudioPlayer::begin(). Повторний виклик безпечний.
    static bool begin();
    static bool isReady();

    // Блокуючий старт (див. «БЛОКУВАННЯ»). true = запит прийнято і ресурси створено; Running настає пізніше,
    // перевіряти state(). false = зайнято, не готово або створення не вдалося (lastError()).
    static bool start();

    // Неблокуючий старт. true = запит прийнято (створення і старт виконуються в задачі); false = зайнято /
    // не готово / нестача памʼяті на буфер. Результат пізніше: state(), lastError().
    static bool requestStart();

    // Запит на зупинку проходу (спад гучності, XSMT low, I2S1 вимкнено). Ресурси лишаються.
    // false, якщо ресурсів нема або запит уже є / зайнято.
    static bool stop();

    // Запит на повернення до радіо: зупинити (якщо йде), GPIO18 -> I2S0, 48 кГц знято, ресурси звільнено.
    // Неблокуючий. XSMT лишається низько (радіо саме зніме мʼют, коли потік стабільний).
    static bool returnToRadio();

    static bool isRunning();     // Starting / Running / Stalled
    static bool isStable();      // state == Running (без внутрішнього мʼюту; мʼют користувача сюди не входить)
    static bool resourcesUp();   // задача й канали існують (між start*/requestStart і returnToRadio)
    static LinePathState state();
    static const char* stateName(LinePathState s);
    static LinePathError lastError();
    static const char* errorName(LinePathError e);

    // Гучність проходу, дБ. Обрізається до [kVolMinDb, kVolMaxDb]. Плавно (через DspVolume, будь-який стан).
    static void setVolumeDb(float db);
    static float volumeDb();

    // Обгортка для сумісності (lp.att): обрізає до [kAttMinDb, kAttMaxDb] і викликає setVolumeDb.
    static void setAttenuationDb(int8_t db);
    static int8_t attenuationDb();   // volumeDb, округлено

    // Мʼют користувача (цифровий, плавний). Не розмʼючує прохід сам і не залежить від станів.
    static void setUserMute(bool mute);
    static bool userMuted();

    // [Prompt 48b] Цифровий баланс проходу, дБ (обрізається до balance_cfg::kMinDb..kMaxDb): додатне = вправо (лівий
    // канал послаблюється), від'ємне = вліво, 0 = без змін. Той самий DspVolume проходу, ПІСЛЯ гучності;
    // перехід 30 мс. Діє й до старту проходу (значення переживає stop / returnToRadio). Будь-яка задача.
    static void setBalanceDb(int8_t db);
    static int8_t balanceDb();

    // EQ входу (власний екземпляр eq::Processor проходу). Діапазон і обрізання — як у фасаді eq.
    static void setEqBandDb(uint8_t band, int8_t db);
    static int8_t getEqBandDb(uint8_t band);
    static void setEqAllDb(const int8_t gains[eq_cfg::kBandCount]);
    static void getEqAllDb(int8_t out[eq_cfg::kBandCount]);
    static void setEqBypass(bool on);
    static bool eqBypass();

    static LinePathStats stats();
    static void resetStats();
};
