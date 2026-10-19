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
// [Prompt 39] У КІНЕЦЬ структури додано inputEnabledMask (маска дозволених входів, відключення
// входів 1..3 з вебу). Формат blob-а v4; blobs v3, v2 і v1 мігруються в load() (усі входи дозволені).

#include <Arduino.h>
#include <stdint.h>

#include "config/defaults.h"
#include "config/eq_config.h"

// [Prompt 46a] Профіль одного логічного входу: гучність (шкала UI 0..100, діапазон обрізає
// AppController за capabilities()) і пресет 5-смугового еквалайзера, дБ (eq_cfg::kGainMinDb..
// kGainMaxDb, 0 = плоска АЧХ; sanitize() у settings.cpp обрізає пошкоджені значення). Мʼют сюди
// НЕ входить. Профіль входу, якого чип не має (вхід 3 для PT2313L), просто зберігається.
// Бас / дискант / баланс / gain / loudness з профілю прибрано (P46a): чип лишається селектором.
struct InputProfile {
    int8_t volume;
    int8_t eqGainsDb[eq_cfg::kBandCount];
};

// Налаштування, які зберігаються між запусками.
// УВАГА: зміна порядку/типів полів → збільшити settings_cfg::kFormatVersion.
struct Settings {
    // Тип аудіопроцесора. Значення відповідає enum class AudioProcType
    // (audio/audio_processor.h): 0 = Tda7318, 1 = Pt2313l.
    // Відповідність перевіряється static_assert у settings.cpp.
    uint8_t processorType;

    // Назви входів (кількість = defaults::kInputCount = 5: 0 радіо, 1..4 зовнішні) [Prompt 46b].
    char inputNames[defaults::kInputCount][32];

    // Яскравість підсвітки дисплея (та сама шкала, що defaults::kDefaultBrightness).
    uint8_t brightness;

    // Орієнтація дисплея: true = display_cfg::kRotationFlipped.
    bool displayFlipped;

    // Останній стан для відновлення.
    uint8_t lastInput;
    uint16_t lastStation;
    bool lastMute;

    // [Prompt 21b] Профілі по входах (індекс = логічний вхід 0..kInputCount-1).
    // [Prompt 46a] Профіль = {volume, eqGainsDb[5]}. Гучність пише AppController (разом з lastInput);
    // пресет EQ входу пише веб-обробник /api/eq (SettingsStore::modify). На екземпляр радіо (eq::*)
    // застосовується лише пресет входу 0 (profiles[0].eqGainsDb): load(), resetToDefaults(), /api/eq,
    // повернення на вхід 0 (AppController).
    InputProfile profiles[defaults::kInputCount];

    // [Prompt 39] Маска дозволених логічних входів: біт i = вхід i (0 = радіо ... 3 = Aux), 1 = дозволено.
    // За замовчуванням дозволені входи 0..3 (settings_cfg::kInputMaskDefault). Біт 0 ЗАВЖДИ 1: нормалізується в
    // load()/sanitize() і в AppController (settings_cfg::normalizeInputMask). Пише лише AppController
    // (команда вебу WebCmdType::InputsEnabledSet). Апаратну доступність входу (PT2313L не має входу 3)
    // маска не змінює: ефективний вхід = апаратно є І біт у масці. Поле стоїть ПІСЛЯ profiles (P46a: глобального eqGainsDb більше немає).
    uint8_t inputEnabledMask;
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
    // негайний save(). [Prompt 46b] МІГРАЦІЙ НЕМАЄ (формат v6; старі blob-и ігноруються). Після load()
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

    // [Prompt 6] ДОДАНО: скільки разів реально писали в NVS з моменту старту.
    static uint32_t writeCount();
};
