#pragma once

// Налаштування в NVS, namespace "audioctl" (спільний з IrRc5, ключі різні).
// [Prompt 6] Реалізовано. У структуру ДОДАНО: displayFlipped, bass, treble,
// balance, loudness (наявні поля не чіпали). Додано методи SettingsStore:
// snapshot, modify, requestSave, flush, isDirty, resetToDefaults, eraseStored,
// writeCount.
// [Prompt 21b] Налаштування звуку (гучність, бас, дискант, баланс, gain, loudness)
// тепер зберігаються ОКРЕМО ДЛЯ КОЖНОГО ВХОДУ: структура InputProfile, масив
// Settings::profiles[defaults::kInputCount]. ВИДАЛЕНО глобальні поля bass, treble,
// balance, loudness, lastVolume (їх роль виконує профіль входу). Формат blob-а v2;
// blob v1 мігрується в load() (див. settings.cpp). lastInput/lastStation/lastMute лишились.
// [Prompt 30] У КІНЕЦЬ структури додано eqGainsDb[5] (глобальний пресет еквалайзера радіо).
// Формат blob-а v3; blob v2 (без еквалайзера) і v1 мігруються в load().
// [Prompt 46a] NVS blob v5: профіль входу = {volume, eqGainsDb[5]} (пресет еквалайзера тепер ПО ВХОДАХ).
// ВИДАЛЕНО: InputProfile::bass/treble/balance/gain/loudness і глобальне Settings::eqGainsDb.
// Міграція v1..v4 -> v5 у load() (глобальний EQ копіюється в профілі всіх входів).
// [Prompt 46b] NVS blob v6: 5 логічних входів (inputNames[5], profiles[5]); БЕЗ МІГРАЦІЇ (старі формати ->
// значення за замовчуванням); ланцюг міграцій v1..v5 з load() видалено.
// [Prompt 48a] NVS blob v7 (БЕЗ міграції): ВИДАЛЕНО processorType (аудіочіпи TDA7318 / PT2313L вирізано);
// 3 логічні входи (inputNames[3], profiles[3]: 0 радіо, 1 TV Box, 2 Computer); профіль входу +balanceDb
// (баланс ±20 дБ, крок 1 дБ, діапазон — config/balance_config.h; у P48a лише зберігається, DSP — P48b).
// [USB14] NVS blob (БЕЗ міграції): profiles[] має player_mode_cfg::kProfileCount = kInputCount + 1 = 4 елементи: 4-й слот —
// профіль входу «Player» (режим «USB» і майбутні джерела SD / BT). defaults::kInputCount (3) НЕ змінено: веб, коло входів
// і маска входів працюють з 0..2; слот 3 бачить лише AppController. У КІНЕЦЬ структури додано playerShuffle / playerRepeat.
// Розкладка: Settings 138 Б, blob 142 Б (було 128 / 132): blob v8 старої довжини відхиляється з логом, діють дефолти.
// Окремо від blob-а (ключі NVS `pl_usb_dir` / `pl_usb_trk` / `pl_usb_pos`, namespace «audioctl») зберігається ПОЗИЦІЯ
// відтворення джерела (SettingsStore::getPlayerResume / setPlayerResume): шляхи на 2 × 256 Б не роздувають blob
// і не копіюються в кожен snapshot().
// [INP4a-1] NVS blob v9 (БЕЗ міграції): inputNames[defaults::kInputSlotCount = 4][32] (слот 3 = вхід «USB», дефолт «Player»),
// inputEnabledMask 4-бітна (біт 3 = USB). Розкладка: Settings 170 Б, blob 174 Б (було 138 / 142). defaults::kInputCount (3)
// НЕ змінено; lastInput допускає 0..defaults::kUsbInputIndex (у цьому кроці його ніхто не ставить у 3).
// [Prompt 39] У КІНЕЦЬ структури додано inputEnabledMask (маска дозволених входів, відключення
// входів 1..3 з вебу). Формат blob-а v4; blobs v3, v2 і v1 мігруються в load() (усі входи дозволені).

#include <Arduino.h>
#include <stdint.h>

#include "config/balance_config.h"
#include "config/defaults.h"
#include "config/eq_config.h"
#include "config/player_mode_config.h"  // [USB14] PlayerSource, player_mode_cfg::kProfileCount
#include "config/vu_sens_config.h"  // [V1] діапазон чутливості VU

// [Prompt 46a] Профіль одного логічного входу: гучність (шкала UI 0..100, діапазон обрізає
// AppController за audio_cfg::kVolumeUiMin..kVolumeUiMax), пресет 5-смугового еквалайзера, дБ
// (eq_cfg::kGainMinDb..kGainMaxDb, 0 = плоска АЧХ; sanitize() у settings.cpp обрізає пошкоджені
// значення) і [P48a] баланс balanceDb, дБ (balance_cfg::kMinDb..kMaxDb, 0 = обидва канали без змін;
// додатне = вправо, лівий послаблюється; поза діапазоном при завантаженні -> 0). Мʼют сюди НЕ входить.
// Бас / дискант / gain / loudness з профілю прибрано (P46a). Поле volume — int8_t, як і раніше.
struct InputProfile {
    int8_t volume;
    int8_t eqGainsDb[eq_cfg::kBandCount];
    int8_t balanceDb;  // [Prompt 48a] ДОДАНО: лише зберігається й валідується (DSP — P48b)
    // [V1] ДОДАНО (лише в кінець профілю; NVS v8): чутливість VU цього входу, дБ (vu_sens_cfg::kMinDb..kMaxDb,
    // 0 = нейтрально). Зсув рівня показу VU, на звук не впливає. Діє на VU, коли вхід активний.
    int8_t vuSensDb;
};

// Налаштування, які зберігаються між запусками.
// УВАГА: зміна порядку/типів полів → збільшити settings_cfg::kFormatVersion.
struct Settings {
    // [Prompt 48a] Поле processorType ВИДАЛЕНО (аудіочіпи вирізано).

    // Назви входів (кількість = defaults::kInputSlotCount = 4: 0 радіо, 1 TV Box, 2 Computer, 3 USB) [Prompt 48a, INP4a-1].
    char inputNames[defaults::kInputSlotCount][32];

    // Яскравість підсвітки дисплея (та сама шкала, що defaults::kDefaultBrightness).
    uint8_t brightness;

    // Орієнтація дисплея: true = display_cfg::kRotationFlipped.
    bool displayFlipped;

    // Останній стан для відновлення. [INP4a-1] lastInput: 0..defaults::kUsbInputIndex (3 = «USB»; пише INP4a-2).
    uint8_t lastInput;
    uint16_t lastStation;
    bool lastMute;

    // [Prompt 21b] Профілі по входах (індекс = логічний вхід 0..kInputCount-1).
    // [Prompt 46a] Профіль = {volume, eqGainsDb[5]}; [P48a] + balanceDb. Гучність пише AppController
    // (разом з lastInput); пресет EQ входу пише веб-обробник /api/eq (SettingsStore::modify); balanceDb
    // у P48a не пише ніхто (P48b). На екземпляр радіо (eq::*) застосовується лише пресет входу 0
    // (profiles[0].eqGainsDb): load(), resetToDefaults(), /api/eq, повернення на вхід 0 (AppController).
    // [USB14] Масив має kInputCount + 1 елементів: індекс kInputCount (player_mode_cfg::kProfileIndex) — профіль «Player».
    InputProfile profiles[player_mode_cfg::kProfileCount];

    // [Prompt 39] Маска дозволених логічних входів: біт i = вхід i (0 = радіо, 1 = TV Box, 2 = Computer),
    // 1 = дозволено. [INP4a-1] 4 біти (біт 3 = USB), за замовчуванням 0x0F (settings_cfg::kInputMaskDefault). Біт 0 ЗАВЖДИ 1:
    // нормалізується в load()/sanitize() і в AppController (settings_cfg::normalizeInputMask). Пише лише
    // AppController (команда вебу WebCmdType::InputsEnabledSet). Поле стоїть ПІСЛЯ profiles.
    uint8_t inputEnabledMask;

    // [USB14] Режим «Плеєр»: перемішування й повтор усього (спільні для джерел). Пише AppController (дії USB_SHUFFLE /
    // USB_REPEAT, SettingsStore::modify), читає при вході в режим. Поля стоять ПІСЛЯ inputEnabledMask (лише в кінець).
    bool playerShuffle;
    bool playerRepeat;
};

// [USB14] Збережена позиція відтворення одного джерела плеєра (папка + імʼя треку в ній + секунда). Живе окремо від Settings
// (див. шапку). dir — шлях папки від кореня флешки («/» — корінь), track — ІМʼЯ файлу в цій папці (без шляху).
struct PlayerResume {
    bool valid;      // true — є що відновлювати
    uint32_t posSec; // абсолютна секунда (для AudioPlayer::playFile)
};

// Потокобезпечність: усі методи беруть внутрішній мʼютекс, КРІМ get() —
// посилання на кеш не можна захистити. get() безпечний для читання/запису з
// однієї задачі (AppController). З інших задач (веб, тест) користуйтеся
// snapshot() / modify().
//
// Як користуватися записом (рекомендація для AppController):
//  - save()        — негайний запис (~кілька мс блокування). Для рідкісних подій.
//  - requestSave() — відкладений запис з дебаунсом: фонова задача запише, коли
//                    зміни стихнуть на kSaveDebounceMs (або минув kSaveMaxDelayMs).
//                    Викликати після зміни полів ЗА СПРАВЖНЬОЮ зміною користувача.
//                    Під час ramp гучності НЕ викликати на кожному кроці —
//                    достатньо один раз після завершення ramp (але і виклик на
//                    кожному кроці не зношує флеш: пишеться лише раз за серію).
//  - flush()       — дописати відкладене негайно (перед standby / перезавантаженням).
class SettingsStore {
public:
    // Створює мʼютекс, перевіряє доступ до NVS, запускає фонову задачу запису.
    // У кеші одразу лежать значення за замовчуванням. false — NVS або мʼютекс
    // недоступні (прошивка може працювати з налаштуваннями лише в RAM).
    static bool begin();

    // Читає blob з NVS. Немає запису / інша версія / інший розмір → значення за замовчуванням +
    // негайний save(). [Prompt 48a] МІГРАЦІЙ НЕМАЄ (формат v7; старі blob-и ігноруються). Після load()
    // пресет входу 0 одразу йде в eq::setAllDb() (екземпляр радіо).
    // true — кеш містить дані з NVS або збережені дефолти; false — запис у NVS не вдався (кеш = дефолти/попередній).
    static bool load();

    // Негайно записати всю структуру. false — NVS не відповіла.
    static bool save();

    // Закешована в RAM копія (без звернення до NVS). Див. примітку про потоки.
    static Settings& get();

    // [Prompt 6] ДОДАНО: копія кешу під мʼютексом (для інших задач).
    static Settings snapshot();

    // [Prompt 6] ДОДАНО: змінити кеш під мʼютексом і позначити його «брудним»
    // (як requestSave()). fn має бути короткою й не викликати SettingsStore.
    static void modify(void (*fn)(Settings&, void* ctx), void* ctx);

    // [Prompt 6] ДОДАНО: відкладений запис (дебаунс).
    static void requestSave();

    // [Prompt 6] ДОДАНО: якщо є відкладені зміни — записати зараз.
    static bool flush();

    // [Prompt 6] ДОДАНО: чи є зміни, ще не записані в NVS.
    static bool isDirty();

    // [Prompt 6] ДОДАНО: дефолти в кеш + негайний запис. Чіпає ЛИШЕ наш ключ,
    // не весь namespace (там лежить мапа IR).
    static bool resetToDefaults();

    // [Prompt 6] ДОДАНО: видалити наш ключ з NVS, кеш не чіпати (для перевірки
    // «першого запуску» після перезавантаження). Скасовує відкладений запис.
    static bool eraseStored();

    // [Prompt 6] ДОДАНО: скільки разів реально писали в NVS з моменту старту (лише blob; ключі позиції плеєра рахує
    // playerResumeWriteCount()).
    static uint32_t writeCount();

    // [USB14] ДОДАНО: позиція відтворення джерела плеєра (кеш у RAM; запис у NVS — відкладений, як requestSave()).
    //  - getPlayerResume: копіює збережені папку й імʼя треку в буфери викликача (обрізання НЕ робиться: false при малому
    //    буфері) і позицію. false — немає збереженого, або малий буфер. Буфери викликача краще мати статичними:
    //    стек задачі usb_nav малий.
    //  - setPlayerResume: оновлює кеш і ПОЗНАЧАЄ «брудним», ЛИШЕ якщо значення відрізняються від збережених (повтор тієї
    //    ж паузи / позиції не пише). Запис виконує фонова задача з тим самим дебаунсом, що й blob (kSaveDebounceMs /
    //    kSaveMaxDelayMs); flush() дописує негайно (перед standby / перезапуском).
    //  - clearPlayerResume: забути позицію (кеш + видалення ключів NVS при наступному записі).
    //  - playerResumeWriteCount: скільки разів реально писали ключі позиції в NVS з моменту старту (діагностика).
    static bool getPlayerResume(PlayerSource src, char* dirOut, size_t dirCap, char* trackOut, size_t trackCap,
                                PlayerResume* info);
    static void setPlayerResume(PlayerSource src, const char* dir, const char* track, uint32_t posSec);
    static void clearPlayerResume(PlayerSource src);
    static uint32_t playerResumeWriteCount();
};


