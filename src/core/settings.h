#pragma once

// Налаштування в NVS, namespace "audioctl" (спільний з IrRc5, ключі різні).
// [Prompt 6] Реалізовано. У структуру ДОДАНО: displayFlipped, bass, treble,
// balance, loudness (наявні поля не чіпали). Додано методи SettingsStore:
// snapshot, modify, requestSave, flush, isDirty, resetToDefaults, eraseStored,
// writeCount.

#include <Arduino.h>
#include <stdint.h>

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

    // Тембр, баланс, тонкомпенсація — у шкалі UI аудіопроцесора
    // (AudioProcessorCapabilities). Крок і межі обрізає драйвер, а не Settings.
    int8_t bass;
    int8_t treble;
    int8_t balance;
    bool loudness;

    // Останній стан для відновлення.
    uint8_t lastInput;
    uint16_t lastStation;
    int8_t lastVolume;
    bool lastMute;
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
    // замовчуванням + негайний save(). true — кеш містить дані з NVS або
    // збережені дефолти; false — запис у NVS не вдався (кеш = дефолти/попередній).
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
