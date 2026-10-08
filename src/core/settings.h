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

#include <Arduino.h>
#include <stdint.h>

#include "config/defaults.h"
#include "config/eq_config.h"

// [Prompt 21b] Профіль звуку одного логічного входу (шкали як в AudioProcessorCapabilities:
// volume 0..100, тембр/баланс у кроках UI, gain — сирі апаратні кроки). Діапазони тут НЕ
// валідуються: їх обрізає AppController за capabilities() поточного чипа. Профіль входу,
// якого чип не має (вхід 3 для PT2313L), просто зберігається. Мʼют сюди НЕ входить.
struct InputProfile {
    int8_t volume;
    int8_t bass;
    int8_t treble;
    int8_t balance;
    int8_t gain;
    bool loudness;
};

// Налаштування, які зберігаються між запусками.
// УВАГА: зміна порядку/типів полів → збільшити settings_cfg::kFormatVersion.
struct Settings {
    // Тип аудіопроцесора. Значення відповідає enum class AudioProcType
    // (audio/audio_processor.h): 0 = Tda7318, 1 = Pt2313l.
    // Відповідність перевіряється static_assert у settings.cpp.
    uint8_t processorType;

    // Назви входів (кількість = defaults::kInputCount).
    char inputNames[4][32];

    // Яскравість підсвітки дисплея (та сама шкала, що defaults::kDefaultBrightness).
    uint8_t brightness;

    // Орієнтація дисплея: true = display_cfg::kRotationFlipped.
    bool displayFlipped;

    // Останній стан для відновлення.
    uint8_t lastInput;
    uint16_t lastStation;
    bool lastMute;

    // [Prompt 21b] Профілі звуку по входах (індекс = логічний вхід 0..kInputCount-1).
    // Профіль lastInput — те, що діє зараз (AppController пише його разом з lastInput).
    InputProfile profiles[defaults::kInputCount];

    // [Prompt 30] Глобальний пресет 5-смугового еквалайзера радіо, дБ
    // (eq_cfg::kGainMinDb..kGainMaxDb, 0 = плоска АЧХ). НЕ по входах і НЕ по станціях.
    // Поле стоїть ПІСЛЯ profiles: зсуву наявних полів немає. Застосовується до eq::* у
    // SettingsStore::load()/resetToDefaults(); змінює веб-обробник /api/eq.
    int8_t eqGainsDb[eq_cfg::kBandCount];
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

    // Читає blob з NVS. Немає запису / інша версія / інший розмір → значення за
    // замовчуванням + негайний save(). [Prompt 21b] Виняток: blob формату v1 мігрується
    // (усі профілі входів = колишні глобальні гучність/тембр/баланс/loudness, gain =
    // settings_cfg::kDefaultInputGain) і одразу записується як v3. [Prompt 30] Blob v2 мігрується
    // так само (еквалайзер = 0 дБ). Після load() пресет еквалайзера одразу йде в eq::setAllDb().
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
