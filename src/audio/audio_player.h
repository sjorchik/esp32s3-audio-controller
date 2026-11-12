#pragma once

// Аудіоплеєр інтернет-радіо: обгортка над ESP32-audioI2S.
//
// - I2S ініціалізує ВИКЛЮЧНО бібліотека (BCLK/WS/DOUT з config/pins.h).
// - Гучність бібліотеки завжди максимальна; гучність цифрова (поза плеєром).
// - Софт-мʼют ЦАП PCM5102 (XSMT, pins::kXsmt) контролює цей модуль. Мʼют
//   цифровий мʼют радіо / проходу цей модуль НЕ викликає — це відповідальність AppController.
// [Prompt 48a] ЗМІНЕНО: begin() БЕЗ параметрів (аудіопроцесор вирізано).
// - Усі команди (playUrl/stop) асинхронні: вони лише ставлять запит, а виконує
//   його задача плеєра (ядро 1) — бібліотека не потокобезпечна.
// - Публічні гетери потокобезпечні (викликати можна з будь-якої задачі).
//
// [Prompt 25] ДОДАНО: setOutputTrimDb()/outputTrimDb() — рівень виходу декодера по станціях.
// [Prompt 44] ДОДАНО: setFixedOutputRate()/fixedOutputRate() — фіксація 48 кГц на I2S0 для line_path.
//
// [DEF6] ДОДАНО: структура FileEofStats і fileEofStats() (лише FEATURE_USB_HOST && USB_HOST_DIAG; лічильники для консолі
// usb.eof). Сторожовий таймер кінця файлу (FILE_EOF_WATCHDOG) публічного інтерфейсу НЕ змінює: синтезований eof піднімає
// той самий прапорець, що takeFileEnded() / fileEndedPending().
// [USB7] ДОДАНО: playFile() і допоміжні (файлове джерело з USB-флешки через Audio::connecttoFS, секція «Файл»
// нижче). Публічний інтерфейс радіо, begin() та сигнатури наявних методів НЕ змінено.
//
// [USB18a] ДОДАНО: seekFileSec(sec, atEnd) (необовʼязковий вихідний параметр; seek за кінець файлу = кінець треку), FileTags і
// fileTags(path, out) (ID3 title / artist / album для веб-API), fileTagStats() (лише USB_HOST_DIAG). Наявне не змінено.
// [Prompt 5] Заглушку замінено. Позначки:
//   ЗМІНЕНО — сигнатура відрізняється від заглушки (розділ 12);
//   ДОДАНО  — нове.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "config/features.h"           // [DEF6] FEATURE_USB_HOST / USB_HOST_DIAG
#include "config/file_player_config.h"  // [DEF6] kEofStatNameMax

// ДОДАНО: стан підключення для UI.
//  Idle         — нічого не відтворюється (початковий стан і після stop()).
//  Connecting   — перша спроба підключення до станції (виклик блокує задачу
//                 плеєра, тому ззовні цей стан видно лише короткочасно).
//  Buffering    — зʼєднання є, наповнюється буфер (старт) або він просів
//                 (недовантаження під час відтворення).
//  Playing      — потік грає.
//  Error        — потік обірвався/не підключився; чекаємо часу наступної спроби.
//  Reconnecting — повторна спроба підключення після Error (виклик у процесі).
enum class PlayerState : uint8_t {
    Idle,
    Connecting,
    Playing,
    Buffering,
    Error,
    Reconnecting,
};

class AudioPlayer {
public:
    // [P48a] begin() без параметрів.
    // Створює обʼєкт Audio, налаштовує I2S (через бібліотеку), встановлює XSMT у
    // мʼют і створює задачу плеєра (ядро 1). Повторний виклик безпечний.
    static bool begin();

    // Запит на відтворення URL (http/https-потік, а також m3u/pls-плейлист,
    // який бібліотека розбирає сама). true — запит прийнято (не означає, що
    // потік уже грає). Новий запит під час відтворення перемикає станцію.
    static bool playUrl(const char* url);

    // Запит на зупинку. true — запит прийнято. Скасовує перепідключення.
    static bool stop();

    // true у станах Playing і Buffering (потік активний; для AppState.streamPlaying).
    static bool isPlaying();

    // Тіло задачі плеєра. Не викликати вручну: його запускає begin().
    static void taskLoop();

    // --- ДОДАНО: стан, метадані, діагностика ---

    static PlayerState state();
    static const char* stateName(PlayerState s);

    // Копіює поточні назву станції та заголовок треку (УЖЕ UTF-8). Повертає
    // true, якщо хоча б один із рядків непорожній. Рядки завжди завершуються
    // '\0' (якщо cap > 0); обрізання — по межі UTF-8-символу.
    static bool currentMetadata(char* stationOut, size_t stationCap,
                                char* titleOut, size_t titleCap);

    // Скільки разів запущено повторне підключення (кожна спроба після Error).
    static uint32_t reconnectCount();

    // Скільки разів буфер просідав під час стабільного відтворення
    // (перехід Playing -> Buffering). Рахується нами за заповненням буфера, а не
    // за callback-ом бібліотеки.
    static uint32_t bufferUnderrunCount();

    // Останній HTTP-код, побачений у повідомленнях бібліотеки; 0 — невідомий.
    // Залежить від того, чи друкує ваша версія бібліотеки рядок "HTTP/1.x NNN".
    static int lastHttpCode();

    // Перекодування ICY-рядка (cp1251 / UTF-8 / Latin-1) в UTF-8 з нормалізацією
    // типографських символів. Публічно, щоб тестовий режим міг перевірити логіку
    // без потоку. Повертає назву визначеного кодування: "ascii", "utf8",
    // "cp1251", "latin1" (або "none"). out завжди завершується '\0'.
    static const char* normalizeIcy(const char* in, char* out, size_t cap);

    // [Prompt 25] ДОДАНО: рівень виходу декодера, дБ — ЦИФРОВЕ послаблення PCM до I2S
    // (вільна функція-хук audio_process_i2s у audio/vu_pcm_hook.cpp через audio/output_trim).
    // Лише послаблення: db обрізається до station_level_cfg::kLevelMinDb..kLevelMaxDb
    // (-24..0), 0 = без змін. Крок 1 дБ; зміна наживо — плавна (≈ десятки мс, без клацань).
    // Можна викликати з будь-якої задачі й до begin(); не чіпає потік і гучність бібліотеки.
    // Рівень НЕ скидається при stop()/перепідключенні: його задає AppController при старті
    // кожної станції.
    static void setOutputTrimDb(int8_t db);

    // [Prompt 25] ДОДАНО: остання встановлена ціль рівня, дБ (початково -6).
    static int8_t outputTrimDb();

    // [Prompt 44] ДОДАНО: примусова фіксована частота I2S0 (Audio::setOutputSampleRate): true —
    // 48 кГц (бібліотека ресемплить потік), false — частота джерела (як було). Потрібна проходу
    // зовнішнього звуку (audio/line_path), якому потрібні відомі такти. Асинхронно: виконує
    // задача плеєра, і ЛИШЕ коли PlayerState::Idle (бібліотека не потокобезпечна). Повертає
    // true, якщо запит прийнято (стан Idle); застосування перевіряти через fixedOutputRate().
    // Режим НЕ скидається при stop(); під час радіо його треба знімати (false).
    static bool setFixedOutputRate(bool fixed48k);

    // [Prompt 44] ДОДАНО: true, якщо фіксацію 48 кГц застосовано (виконано задачею плеєра).
    static bool fixedOutputRate();

    // [Prompt X1] ДОДАНО: true, якщо плеєр востаннє ЗАПИТАВ XSMT HIGH (ЦАП розмʼючено). Не вимір піна
    // (прохід line_path пише в той самий пін самостійно). Будь-яка задача.
    static bool dacUnmuted();

    // [Prompt X1] ДОДАНО: негайно підтягнути XSMT до LOW (мʼют ЦАП) з будь-якої задачі, без очікувань.
    // Для «тихого» перезапуску: гарантує XSMT LOW перед ESP.restart(). Стан плеєра не змінює.
    static void forceDacMute();

    // ------------------------------------------------------------------------------------------------
    // [USB7] Файл з USB-флешки (/usb через usb_fs::volume()). Усі команди асинхронні (виконує задача плеєра).
    // Керування лише з діагностичної консолі (USB_HOST_DIAG); режим «USB», навігація, автоперехід — крок g.
    // ------------------------------------------------------------------------------------------------

    // Запит на відтворення файлу. path — БЕЗ точки монтування, з '/' на початку (напр. "/song.mp3"; Audio::connecttoFS
    // сам додасть '/' за відсутності). startSec >= 0 — стартова секунда (connecttoFS, абсолютна), -1 — з початку.
    // Передумови (інакше false і рядок «[PLAYER] playFile refused: ...» у Serial): плеєр запущено, том USB
    // змонтовано (usb_msc_mount::isMounted()), радіо НЕ грає / не запитане, прохід line_path не піднято і I2S0 не
    // зафіксовано на 48 кГц (перемикання режимів — крок g). Якщо вже грає інший ФАЙЛ — він зупиняється штатним шляхом
    // (мʼют ЦАП + пауза, stopSong). true — запит прийнято, не означає, що звук уже є (стежити за state() / usb.pstat).
    // Рівень виходу (output_trim) на час файлу = file_player_cfg::kFileLevelDb (ПРИПУЩЕННЯ, див. hwcheck_USB7.md);
    // гучність користувача — той самий radio_volume (веде AppController, плеєр його не чіпає).
    static bool playFile(const char* path, int32_t startSec = -1);

    // true — поточне джерело плеєра файл (активний АБО завершений / зупинений до наступного playUrl / playFile).
    static bool isFileSource();

    // true — файл відкрито й відтворюється (стани Buffering / Playing, до кінця / зупинки / збою).
    static bool fileActive();

    // Прапорець «файл закінчився природно (eof)». Піднімається ПІСЛЯ паузи kEndTailMs, мʼюту ЦАП і переходу в Idle;
    // скидається при старті НОВОГО файлу. НЕ піднімається при stop(), збої читання й вийнятті флешки (тоді стан
    // Error + lastFileError()). takeFileEnded() — читання зі скиданням; fileEndedPending() — лише перегляд.
    // Автопереходу на наступний трек НЕМАЄ.
    static bool takeFileEnded();
    static bool fileEndedPending();
    // millis() моменту, коли піднято прапорець (0 — не піднімався).
    static uint32_t fileEndedAtMs();

    // Тривалість і позиція, секунди. Кеш, який оновлює задача плеєра кожні file_player_cfg::kPosPollMs
    // (Audio::getAudioFileDuration / getAudioCurrentTime); 0 — невідомо / файл не грає. Роздільність 1 с.
    static uint32_t fileDurationSec();
    static uint32_t filePositionSec();

    // Запит на зсув позиції: тонка обгортка над Audio::setTimeOffset(int). ВІДНОСНИЙ зсув у секундах (знак — напрям):
    // підтверджено на залізі (USB7, партія 2: +30 з 6 с -> 37 с, -10 з 41 с -> 32 с). Точність у межах ~1 с на CBR
    // 128 кбіт/с; VBR не вимірювалось. false — файл не грає.
    // [USB18a] Якщо sec > 0, тривалість відома (> 0) і (кеш позиції + sec) >= тривалість - kSeekEndMarginSec — бібліотечний seek НЕ
    // виконується: файл завершується як звичайний кінець треку (той самий шлях, що синтезований eof з DEF6: takeFileEnded()
    // підніметься за ≈ kEndTailMs, черга перейде на наступний трек; без збою й без внеску в послідовність пропусків), а
    // *atEnd = true (atEnd може бути nullptr). Рішення ухвалюється за кешем (роздільність 1 с); sec <= 0 або тривалість
    // невідома — поведінка як раніше. false — файл не грає.
    static bool seekFileSec(int32_t sec, bool* atEnd = nullptr);

    // [USB7] Налаштування на льоту для підбору клацань на старті (консоль `usb.fx`; діють на НАСТУПНИЙ файл).
    //  extraMs  — додаткова пауза перед XSMT HIGH (поверх xsmt::clockSettleMs());
    //  digMs    — пауза між XSMT HIGH і цифровим розмʼютом radio_volume; -1 = розмʼютити радіо-DSP одразу при
    //             старті файлу (поведінка до виправлення, коли ЦАП відкривався посеред сигналу);
    //  levelDb  — рівень виходу декодера для файлу, дБ (station_level_cfg::kLevelMinDb..kLevelMaxDb).
    // Значення обрізаються до допустимих меж. Будь-яка задача.
    static void setFileTune(uint32_t extraMs, int32_t digMs, int levelDb);
    static void getFileTune(uint32_t* extraMs, int32_t* digMs, int* levelDb);

    // Копіює шлях останнього запитаного файлу (порожній рядок, якщо файлу не було). true — шлях непорожній.
    static bool currentFilePath(char* out, size_t cap);

    // [USB18a] ID3-теги файлу (UTF-8, кожен <= file_player_cfg::kTagMaxBytes - 1 байтів; порожній рядок — тегу немає, він не
    // зустрівся ще або містив недійсний UTF-8 — перекодування cp1251 / UTF-16 НЕ робиться). Скидаються на старті кожного
    // файлу. Будь-яка задача; короткий критичний розділ (без блокувань довше за копіювання ≈ 200 Б).
    struct FileTags {
        char title[file_player_cfg::kTagMaxBytes];
        char artist[file_player_cfg::kTagMaxBytes];
        char album[file_player_cfg::kTagMaxBytes];
    };
    // Копіює теги, ЯКЩО вони належать файлу path (той самий рядок, що передано в playFile; порівняння за хешем шляху — тож теги
    // попереднього файлу не «протікають» у вікні між запитом і стартом нового). true — теги цього файлу (можливо порожні).
    static bool fileTags(const char* path, FileTags* out);

    // Причина останнього збою файлу (статичний рядок) або nullptr. Скидається при старті нового файлу.
    static const char* lastFileError();

    // ------------------------------------------------------------------------------------------------
    // [USB14] Пауза файлу. Бібліотечний pauseResume() лише перемикає прапорець і очищає вихідні буфери, ЦАП НЕ мʼютить, тож
    // власна послідовність (задача плеєра): цифровий мʼют radio_volume (плавно) -> XSMT LOW (+ kMuteLeadMs) -> pauseResume().
    // Продовження: цифровий мʼют + стан «ЦАП закритий» як на старті файлу, pauseResume(), далі той самий розмʼют, що на
    // старті (XSMT HIGH після kClockSettleMs від моменту продовження, цифровий розмʼют через kDigitalUnmuteDelayMs,
    // нарощування звуку робить DSP). Пауза НЕ є збоєм: fileActive() лишається true, state() = Playing, «decoder stopped» не
    // спрацьовує, позиція зберігається (filePositionSec() не змінюється). Зупинка / новий файл / збій / вийняття флешки
    // скидають паузу (filePaused() = false).
    // ------------------------------------------------------------------------------------------------

    // Запит на паузу (true) / продовження (false). Асинхронно (виконує задача плеєра). true — запит прийнято. Відмова
    // (false + рядок «[PLAYER] pause refused: ...»): файл не грає, ще не в Playing (старт), іде природний кінець, або стан
    // уже такий. Будь-яка задача.
    static bool setFilePaused(bool paused);

    // true — файл на паузі (виставляє задача плеєра ПІСЛЯ виконання паузи; скидається при продовженні / зупинці / збої).
    static bool filePaused();

#if FEATURE_USB_HOST && USB_HOST_DIAG
    // [DEF6] Лічильники кінців файлу (консоль `usb.eof`). real — справжні eof від бібліотеки (оброблені плеєром), synth —
    // синтезовані сторожовим таймером, stallNoAction — завмирання біля кінця, виявлені при FILE_EOF_WATCHDOG = 0 (без дій).
    // haveLast — був хоч один випадок завмирання; lastSynthesized — він закінчився синтезом (інакше «без дій»).
    // Значення пише задача плеєра; читання з іншої задачі без блокувань (діагностика: можливе неузгоджене читання).
    struct FileEofStats {
        uint32_t real;
        uint32_t synth;
        uint32_t stallNoAction;
        bool watchdogOn;
        bool haveLast;
        bool lastSynthesized;
        uint32_t lastCur;      // секунди
        uint32_t lastDur;      // секунди
        uint32_t lastInbuf;    // байти у вхідному буфері бібліотеки
        uint32_t lastStalledMs;
        char lastName[file_player_cfg::kEofStatNameMax];
    };
    static void fileEofStats(FileEofStats* out);
#endif

#if FEATURE_USB_HOST && USB_HOST_DIAG
    // [USB18a] Лічильники: відхилені теги з недійсним UTF-8 і seek-и за кінець файлу (atEnd). Без блокувань (діагностика).
    static void fileTagStats(uint32_t* rejected, uint32_t* seekEnd);
#endif

    // [Prompt 17] Декодовані PCM-семпли для VU перехоплює вільна функція
    // audio_process_i2s(int32_t*, int16_t, bool*) в audio/vu_pcm_hook.cpp (за VU_PCM_HOOK_STYLE
    // та ENABLE_VU) і передає піки в VuSourceDecodedPcm::publishPeaks(). Публічний
    // інтерфейс AudioPlayer не змінено.
};

