# MASTER SPEC — ESP32-S3 Audio Controller

> Вставляти на початку КОЖНОГО промпту для чату з кодом. Після завершення кожного модуля оновлювати розділи 11, 12 і 13.

---

## 1. Загальне

Аудіо-контролер: інтернет-радіо + зовнішні аналогові входи, керування кнопками, енкодером та IR-пультом, кольоровий дисплей, веб-інтерфейс налаштувань.

- Збірка: VS Code + PlatformIO, Arduino framework.
- Мова коментарів у коді — українська, ідентифікатори (змінні, функції, файли) — англійська.
- Повідомлення в Serial-логах — англійською, короткі, з тегом модуля, наприклад `[IR] ...`.
- Репозиторій: https://github.com/sjorchik/esp32s3-audio-controller

## 2. ПРАВИЛА ВІДПОВІДІ (обовʼязкові)

1. Завжди видавай ПОВНИЙ текст кожного файлу, який створюється або змінюється. Ніяких фрагментів «встав сюди», «решта без змін», «...».
2. Перед кодом дай список файлів: шлях + «створено» або «змінено».
3. Не змінюй публічні інтерфейси інших модулів. Якщо це необхідно — явно попередь, опиши зміну і видай повні тексти всіх файлів, яких вона торкається.
4. Усі піни, таймінги, розміри буферів і прапорці — лише через `config/`, без «магічних» чисел у модулях. Існуючі `config/pins.h`, `config/features.h`, `config/defaults.h` НЕ переписуй і не дописуй у них: їх повний вміст наведено в розділі 13, використовуй ці імена точно (`pins::kBtnUp`, `defaults::kEventQueueSize`, `ENABLE_VU` тощо). Константи нового модуля клади в НОВИЙ файл `config/<module>_config.h` у тому самому стилі: `constexpr` у власному namespace (`<module>_cfg`), імена `kPascalCase`; лише прапорці умовної компіляції — `#define UPPER_CASE`. Кожен `.h` починається з `#pragma once`.
5. Не роби більше, ніж просить конкретний промпт. Модулі, які не входять у завдання, не реалізуй.
6. Використовуй лише ті версії бібліотек і тулчейн, що вказані в розділі 11. Якщо потрібна інша версія або нова залежність — попередь окремим пунктом і покажи повний фрагмент `platformio.ini` (повний файл).
7. Не використовуй блокуючі `delay()` у задачах, крім явно обґрунтованих випадків. Використовуй FreeRTOS-примітиви (черги, таймери, `vTaskDelay`).
8. Великі структури, спрайти й буфери — у PSRAM (`ps_malloc` або відповідні прапорці бібліотек), а не у внутрішній RAM.
9. IDF 5.x: використовуй ТІЛЬКИ нові драйвери — PCNT (`driver/pulse_cnt.h`), RMT (`driver/rmt_rx.h`, `driver/rmt_tx.h`), LEDC через `ledcAttach()`. Старі API (`driver/pcnt.h`, legacy RMT, `ledcSetup`/`ledcAttachPin`) не використовувати. I2S напряму не чіпати: ним володіє ESP32-audioI2S.
10. Сигнатури заглушок СВОГО модуля (розділ 12) можна змінювати, якщо цього вимагає завдання, але повідом про це окремим пунктом і видай повні `.h`. Заглушки чужих модулів не змінюй.
11. Наприкінці відповіді: розділ «Як перевірити» (що побачу в Serial / на дисплеї / у поведінці пристрою) і розділ «Відомі обмеження».
12. Якщо для завдання бракує відомостей — спочатку постав уточнювальні питання (не більше 3), а не вигадуй припущень мовчки.

## 3. Залізо

- **MCU:** ESP32-S3 N16R8 (16 МБ flash, 8 МБ octal PSRAM). Заборонені піни: GPIO 26–37 (flash/PSRAM), 19/20 (USB).
- **ЦАП:** PCM5102 (I2S). SCK підключений на GND (внутрішній PLL, MCLK не потрібен). XSMT — софт-мʼют, active-low, зовнішній pulldown 10 кОм (при старті пристрій замʼючений).
- **Аудіопроцесор:** TDA7318 АБО PT2313L (I2C, адреса 0x44, зчитування немає, автовизначення неможливе). Вибір типу зберігається в NVS, змінюється у веб-інтерфейсі.
- **Входи процесора:** 0 = WiFi Radio, 1 = TV Box, 2 = Computer, 3 = Aux.
- **Дисплей:** ST7789 170×320, SPI2 (FSPI, піни IOMUX), горизонтальна орієнтація 320×170. Обовʼязково врахувати зміщення вікна (offset) для панелі 170 пікселів. Підсвітка — LEDC PWM.
- **Керування:** 6 кнопок (POWER, UP, DOWN, LEFT, RIGHT, OK), енкодер з кнопкою (апаратний PCNT), IR-приймач VS1838B (RMT RX, вихід active-low).
- **IR-протокол:** RC5 / RC5X (біт toggle; у RC5X 7-й біт команди береться з інвертованого S2). Кнопки пульта навчаються користувачем (режим навчання), коди зберігаються в NVS.
- **PCM1808 (ADC):** у версії 1 НЕ підключений. Піни MCLK і DIN зарезервовані й не драйвляться. VU-метр рахується з декодованих PCM-семплів (працює лише в режимі радіо). Архітектура має мати абстракцію «джерело рівня VU», щоб пізніше додати ADC без переробки UI.
- **Резерв:** UART1 (TX 42, RX 48) — майбутній Bluetooth-модуль; у версії 1 не використовується.
- **Живлення:** одне джерело 12 В → 9 В (аудіопроцесор), 5 В, 3.3 В.

## 4. Мапа пінів (єдине джерело — `config/pins.h`)

| Блок | Сигнал | GPIO | Примітка |
|---|---|---:|---|
| ST7789 (SPI2/FSPI) | SCLK | 12 | IOMUX |
| | MOSI | 11 | IOMUX |
| | CS | 10 | IOMUX |
| | DC | 9 | |
| | RST | 13 | |
| | BLK | 14 | LEDC PWM |
| I2S (PCM5102) | BCLK | 15 | |
| | WS / LRCLK | 17 | |
| | DOUT → DIN PCM5102 | 18 | |
| | MCLK | 16 | резерв (PCM1808), не драйвити |
| | DIN ← PCM1808 | 8 | резерв, не використовується |
| PCM5102 | XSMT | 45 | active-low, pulldown 10 кОм |
| I2C | SDA | 1 | TDA7318 / PT2313L |
| | SCL | 2 | |
| Енкодер | A | 4 | PCNT |
| | B | 5 | PCNT |
| | BTN | 6 | |
| Кнопки | POWER | 7 | |
| | UP | 21 | |
| | DOWN | 38 | |
| | LEFT | 39 | |
| | RIGHT | 40 | |
| | OK | 41 | |
| IR | VS1838B OUT | 47 | RMT RX |
| Резерв BT | UART1 TX | 42 | майбутній модуль |
| | UART1 RX | 48 | |
| Консоль | UART0 TX | 43 | |
| | UART0 RX | 44 | |

Піни не змінювати без прямої вказівки. Якщо модуль потребує нового піна — зупинись і запитай.

## 5. Програмна архітектура

**Подієва шина.** Єдина FreeRTOS-черга подій (`EventBus`). Джерела (кнопки, енкодер, IR, веб) → логічні `Action` → `AppController` → модулі. Модулі не викликають один одного напряму для керування, лише через події й `AppState`.

**Логічні Action:** див. `core/events.h` у розділі 12. Кожна подія має прапорець `repeat` (утримання) і джерело (`EventSource`).

**Розкладка керування за замовчуванням** (реалізується в `AppController`):

| Дія | Radio / зовнішній вхід | Меню |
|---|---|---|
| Енкодер обертається | гучність | навігація по пунктах / зміна значення |
| Енкодер натиснуто | мʼют | вибір |
| UP / DOWN | попередня / наступна станція | навігація |
| LEFT / RIGHT | попередній / наступний вхід | зміна значення |
| OK | відкрити меню | вибір |
| POWER | standby вкл/викл | standby вкл/викл |

**AppState.** Єдине сховище стану: режим, поточний вхід, гучність, тембр, баланс, мʼют, індекс станції, метадані, статус Wi-Fi/потоку, рівні VU. Доступ потокобезпечний.

**Режими:** `Standby` (мʼют, підсвітка вимкнена, потік зупинений, Wi-Fi і веб працюють), `Radio`, `ExternalInput`, `Menu`, `IrLearn`, `WifiSetup`.

**Задачі FreeRTOS.** Аудіо — окреме ядро від UI і мережі. UI малює зі спрайту (double buffering у PSRAM), без блокувань аудіо. Ядра й пріоритети задач — константи в config.

**Ядра й пріоритети задач (фіксовано):**
- Ядро 0: введення (кнопки, енкодер, IR), AppController, UI/дисплей, Wi-Fi, веб-сервер.
- Ядро 1: аудіо (ESP32-audioI2S, декодування, I2S). На ядрі 1, крім аудіозадачі, нічого не запускати; Arduino `loopTask` там теж є (ядро 1, пріоритет 1) — до появи AppController це не заважає аудіо (нижчий пріоритет, задача майже завжди спить), але після AppController `loop()` лишається порожнім і нічого туди не додається.
- **Зайняті пріоритети (ядро 0):** введення (кнопки, енкодер) = 4 (`input_cfg::kBtnTaskPriority` / `kEncTaskPriority`).
- **Зарезервовано:** аудіо (ядро 1) — довільне число, ВИЩЕ за 4 (визначається у Prompt аудіо, не звірене з ядром 0). AppController — нижче за 4, вище за UI (орієнтовно 3). UI/дисплей — нижче за AppController (орієнтовно 2). Мережа/веб — найнижчий (орієнтовно 1).
- Кожен наступний модуль порівнює своє число з цією таблицею й одразу після реалізації оновлює її фактичним значенням.
- Кожна задача періодично віддає процесор (vTaskDelay / блокуюче очікування черги); задачі не мають годувати watchdog вручну.

**Збереження.** Налаштування — NVS (namespace `audioctl`). Станції, шрифти, іконки, веб-сторінки — LittleFS.

**Флеш 16 МБ.** Таблиця розділів: 2 × OTA + LittleFS. Board: `esp32-s3-devkitc-1`, `board_build.arduino.memory_type = qio_opi`, консоль на UART0.

## 6. Модулі (папки `src/`)

| Папка | Файли / відповідальність |
|---|---|
| `config/` | `pins.h`, `features.h`, `defaults.h`, `<module>_config.h` |
| `core/` | `events`, `app_state`, `app_controller`, `settings` (NVS) |
| `input/` | `buttons`, `encoder` (PCNT), `ir_rc5` (RMT, декодер, навчання) |
| `audio/` | `audio_player` (ESP32-audioI2S), `audio_processor.h` (інтерфейс), `tda7318`, `pt2313l`, `vu_source` |
| `stations/` | `station_store` (LittleFS JSON), імпорт M3U / PLS / JSON |
| `ui/` | `display` (LovyanGFX), `fonts` (укр.), `icons`, `screens` |
| `net/` | `wifi_manager` (STA + AP provisioning + captive portal), `web_server`, `mdns` |
| корінь `src/` | `main.cpp` |

## 7. Інтерфейс аудіопроцесора

Спільний абстрактний інтерфейс для TDA7318 і PT2313L (актуальна заглушка — у розділі 12):

- `begin()`, `setInput(index)`, `setVolume(value)`, `setBass(value)`, `setTreble(value)`, `setBalance(value)`, `setMute(bool)`, `setLoudness(bool)` (якщо підтримується);
- `capabilities()` — що підтримується (тембр, баланс, loudness, кількість входів, діапазони);
- UI ховає непідтримувані функції;
- плавна зміна гучності (ramp) і мʼют під час перемикання входу та зміни станції (XSMT + аттенюатор процесора);
- рівні I2C: підтяжки до 3.3 В, швидкість 100 кГц; при живленні процесора 9 В звірити пороги логічної одиниці за даташитом.

## 8. Wi-Fi provisioning

- Немає збереженої мережі або 3 невдалих спроби підключення → режим AP з назвою `AudioCtrl-Setup`, captive portal (DNS), сторінка: скан мереж, вибір, пароль → збереження в NVS → перезапуск у режим STA.
- На дисплеї в цей час — інструкція (назва AP, адреса).
- Скидання Wi-Fi — утримання кнопки OK під час старту.
- Та сама веб-частина, що й для налаштувань: після підключення до мережі доступний повний інтерфейс за адресою mDNS (`audio.local`) і IP.

## 9. Веб-інтерфейс (перелік можливостей)

Стан (`/api/status`), налаштування (тип процесора, назви входів, яскравість, Wi-Fi), станції (перегляд, редагування, імпорт JSON/M3U/PLS, експорт), навчання кнопок IR, OTA-оновлення прошивки. Сторінки зберігаються в LittleFS (стиснуті gzip).

## 10. Українська мова та іконки на дисплеї

- Повний український алфавіт, включно з `Є є І і Ї ї Ґ ґ`. Покриття шрифта потрібно ПЕРЕВІРИТИ; якщо якихось літер немає (наприклад, `Ґ ґ`) — згенерувати власний шрифт із TTF.
- Метадані ICY можуть приходити в UTF-8, cp1251 або Latin-1: потрібне визначення кодування й конвертація в UTF-8.
- Довгі назви станцій і заголовки — прокрутка (marquee) без мерехтіння.
- Іконки (Wi-Fi, мʼют, гучність, входи, standby тощо) — бітмапи в одному наборі, RGB565 або 1-біт, зберігаються централізовано в `ui/icons`.
- Розкладка екрана (VU, назва станції, метадані) підбирається експериментально: усі координати й розміри виносити в константи.

## 11. Поточний стан проекту (оновлювати вручну)

**Тулчейн:** pioarduino 55.03.37 (Arduino-ESP32 3.3.7 / IDF 5.5.2), board `esp32-s3-devkitc-1`, `qio_opi`, консоль UART0, USB CDC вимкнено.

**Бібліотеки:** LovyanGFX ^1.2.0, ESP32-audioI2S (git), ESPAsyncWebServer + AsyncTCP (ESP32Async, git), ArduinoJson ^7.4.1. `lib_ldf_mode = chain`.

**Виконані модулі:**
- Скелет проекту (platformio.ini, partitions.csv, config/*, заглушки модулів, main.cpp).
- `core/events.h` (`EventBus`) — реалізовано.
- `input/buttons`, `input/encoder`, `config/input_config.h` — реалізовано (Prompt 1). Деталі нижче й у розділах 12–13.

**Важливо для AppController (ще не написаний), врахувати при реалізації:**
- `Action::ENC_PRESS` з `longPress=false` (коротке натискання кнопки енкодера) призначати на mute; `longPress=true` (утримання ≥ `kBtnLongPressMs`) — на іншу дію (наприклад, вхід у меню). Коротка подія приходить лише при відпусканні і не дублюється після довгої.
- Аналогічно для POWER і OK: обидві мають `kBtnLongPress* = true`, тобто в них теж є окремі довгі події (за замовчуванням короткий POWER/OK поки нічого не займає в AppController — розподілити дії при написанні).
- Демо-друк подій у `main.cpp` (`INPUT_DEMO_PRINT_EVENTS`) читає з тієї самої черги `EventBus`, що й майбутній `AppController`. Вимкнути прапорець у `input_config.h` (`#define INPUT_DEMO_PRINT_EVENTS 0`), коли AppController підключить власне читання черги — інакше вони конкуруватимуть за події.
- `Buttons::droppedEvents()` / `Encoder::droppedEvents()` — лічильники втрачених подій (переповнена черга). Варто вивести кудись у діагностику/веб-статус.

**Заплановані зміни інтерфейсів (виконуються у відповідних промптах):**
- `Action`: додати IR-дії `VOL_UP`, `VOL_DOWN`, `MUTE`, `MENU`, `BACK`, `INPUT_RADIO`, `INPUT_TV`, `INPUT_PC`, `INPUT_AUX`, `DIGIT_0..DIGIT_9` (Prompt 2). `Event` більше не змінюється без потреби — поля `longPress`/`delta` вже додані.
- `IrRc5`: новий інтерфейс з навчанням замість заглушки (Prompt 2).
- `AudioProcessor`: додати `probe()`, `applyAll()`, розширені capabilities (фейдер, підсилення входу) (Prompt 3).
- `AppState`: додати безпечне часткове оновлення (`modify`) при реалізації AppState.
- `Settings`: додати `bass`, `treble`, `balance`, `loudness`; `processorType`: 0 = TDA7318, 1 = PT2313L.

**Відомі особливості реалізації (не проблеми, але важливо знати):**
- PCNT: поле `accum_count` у `pcnt_unit_config_t` — без `flags.` у IDF 5.5.2 (на відміну від 5.2.x). Якщо після оновлення тулчейну збірка видасть `no member named 'flags'` — прибрати `flags.`.
- Дебаунс за часом дає затримку: коротка подія приходить приблизно через `kBtnDebounceMs` (30 мс) після фізичного відпускання; довге натискання спрацьовує приблизно через `kBtnLongPressMs` (800 мс) від початку натискання, плюс дебаунс.
- Поки кнопка утримується, повтор і довге натискання взаємовиключні (що настало першим — те й діє). З поточними прапорцями конфлікту немає: повтор лише в UP/DOWN/LEFT/RIGHT, довге — лише в POWER/OK/кнопки енкодера.
- Кнопка, яка вже утримувалась на момент старту задачі `Buttons`, ігнорується до першого відпускання (так утримання OK на старті для скидання Wi-Fi не породжує хибних подій).
- `delta` обмежене типом `int8_t` (максимум 127); надлишок відкидається, фізично недосяжно.
- Акселерація енкодера одноступенева (один множник), скидається зміною напрямку або паузою довшою за поріг.

**Відомі проблеми:** поки немає.

## 12. Інтерфейси готових модулів (оновлювати вручну)

Нижче лише публічні інтерфейси: реалізації опущено.

### `core/events.h` — реалізовано

```cpp
#pragma once

// Подієва шина.
// Єдина FreeRTOS-черга подій.
// Джерела: кнопки, енкодер, IR, веб.
// Модулі не викликають один одного напряму для керування.

#include <Arduino.h>
#include <stdint.h>

// Логічні дії.
enum class Action : uint8_t {
    POWER,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    OK,
    ENC_CW,
    ENC_CCW,
    ENC_PRESS,
};

// Джерело події.
enum class EventSource : uint8_t {
    BUTTON,
    ENCODER,
    IR,
    WEB,
};

// Подія.
// Прапорець repeat означає утримання.
struct Event {
    Action action;
    EventSource source;
    bool repeat;

    // Довге натискання: одна подія в момент досягнення порога утримання.
    // Для repeat-подій і коротких натискань завжди false.
    bool longPress = false;

    // Кількість кроків енкодера (з урахуванням акселерації).
    // Завжди >= 0: напрямок задає Action (ENC_CW / ENC_CCW).
    // Для подій, не повʼязаних з обертанням енкодера, 0.
    int8_t delta = 0;
};

// Мінімальна підготовка шини подій.
// Реалізація логіки обробки буде в AppController.
class EventBus {
public:
    // Створює чергу подій.
    static bool begin();

    // Надіслати подію.
    // Для виклику з ISR використовувати пост-варіант без блокування.
    static bool post(const Event& event, TickType_t timeout = 0);

    // Отримати подію.
    // Використовується задачею обробки.
    static bool poll(Event& event, TickType_t timeout = portMAX_DELAY);

    // Чи шина ініціалізована.
    static bool isReady();
};
```

### `core/app_state.h` — інтерфейс готовий, реалізація попереду

```cpp
#pragma once
#include <Arduino.h>
#include <stdint.h>

enum class Mode : uint8_t {
    Standby, Radio, ExternalInput, Menu, IrLearn, WifiSetup,
};

struct AppStateData {
    Mode mode;
    uint8_t inputIndex;
    int8_t volume;
    int8_t bass;
    int8_t treble;
    int8_t balance;
    bool mute;
    uint16_t stationIndex;
    char stationName[64];
    char trackTitle[128];
    bool wifiConnected;
    bool streamPlaying;
    float vuLeft;
    float vuRight;
};

class AppState {
public:
    static bool begin();
    static AppStateData snapshot();
    static void update(const AppStateData& data);
};
```

### `core/app_controller.h` — інтерфейс готовий, реалізація попереду

```cpp
#pragma once
#include "core/events.h"

class AppController {
public:
    static bool begin();
    static void handleEvent(const Event& event);
};
```

### `core/settings.h` — інтерфейс готовий, реалізація попереду

```cpp
#pragma once
#include <Arduino.h>
#include <stdint.h>

struct Settings {
    uint8_t processorType;
    char inputNames[4][32];
    uint8_t brightness;
    uint8_t lastInput;
    uint16_t lastStation;
    int8_t lastVolume;
    bool lastMute;
};

class SettingsStore {
public:
    static bool begin();
    static bool load();
    static bool save();
    static Settings& get();
};
```

### `audio/audio_processor.h` — абстрактний інтерфейс готовий

```cpp
#pragma once
#include <stdint.h>

struct AudioProcessorCapabilities {
    bool bass;
    bool treble;
    bool balance;
    bool loudness;
    uint8_t inputCount;
    int8_t volumeMin;
    int8_t volumeMax;
    int8_t toneMin;
    int8_t toneMax;
};

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual bool begin() = 0;
    virtual bool setInput(uint8_t index) = 0;
    virtual bool setVolume(int8_t value) = 0;
    virtual bool setBass(int8_t value) = 0;
    virtual bool setTreble(int8_t value) = 0;
    virtual bool setBalance(int8_t value) = 0;
    virtual bool setMute(bool mute) = 0;
    virtual bool setLoudness(bool on) = 0;
    virtual AudioProcessorCapabilities capabilities() const = 0;
};
```

### `audio/vu_source.h` — абстрактний інтерфейс готовий

```cpp
#pragma once
#include <stdint.h>

class VuSource {
public:
    virtual ~VuSource() = default;
    virtual bool read(float& left, float& right) = 0;
};

class VuSourceDecodedPcm : public VuSource {
public:
    bool read(float& left, float& right) override;
};
```

### `audio/tda7318.h`, `audio/pt2313l.h`, `audio/audio_player.h` — заглушки

```cpp
// tda7318.h
#pragma once
#include "audio/audio_processor.h"
class Tda7318 : public AudioProcessor {
public:
    bool begin() override;
    bool setInput(uint8_t index) override;
    bool setVolume(int8_t value) override;
    bool setBass(int8_t value) override;
    bool setTreble(int8_t value) override;
    bool setBalance(int8_t value) override;
    bool setMute(bool mute) override;
    bool setLoudness(bool on) override;
    AudioProcessorCapabilities capabilities() const override;
};

// pt2313l.h
#pragma once
#include "audio/audio_processor.h"
class Pt2313l : public AudioProcessor {
public:
    bool begin() override;
    bool setInput(uint8_t index) override;
    bool setVolume(int8_t value) override;
    bool setBass(int8_t value) override;
    bool setTreble(int8_t value) override;
    bool setBalance(int8_t value) override;
    bool setMute(bool mute) override;
    bool setLoudness(bool on) override;
    AudioProcessorCapabilities capabilities() const override;
};

// audio_player.h
#pragma once
#include <Arduino.h>
class AudioPlayer {
public:
    static bool begin();
    static bool playUrl(const char* url);
    static bool stop();
    static bool isPlaying();
    static void taskLoop();
};
```

### `stations/station_store.h` — заглушка

```cpp
#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

struct Station {
    uint16_t id;
    char name[64];
    char url[192];
};

class StationStore {
public:
    static bool begin();
    static size_t count();
    static bool get(size_t index, Station& out);
    static bool importM3u(const char* path);
    static bool importPls(const char* path);
    static bool importJson(const char* path);
    static bool exportJson(const char* path);
};
```

### `input/buttons.h` — реалізовано (Prompt 1)

```cpp
#pragma once
#include <stdint.h>

// Кнопки: POWER, UP, DOWN, LEFT, RIGHT, OK та кнопка енкодера.
// Опитування в окремій задачі FreeRTOS, події йдуть у EventBus.
class Buttons {
public:
    // Налаштовує піни й створює задачу опитування. Потребує готового EventBus.
    // Повторний виклик безпечний (повертає true, друга задача не створюється).
    static bool begin();

    // Скільки подій кнопок відкинуто через переповнену чергу EventBus.
    static uint32_t droppedEvents();

    // true, якщо пін безперервно тримається в активному стані (LOW) `ms` мс.
    // Працює ДО begin() і без EventBus (сам налаштовує пін як INPUT_PULLUP).
    // Блокує викликаючу задачу на час перевірки (до `ms` мс), тому
    // викликати лише зі setup().
    static bool isHeldAtBoot(uint8_t pin, uint32_t ms);
};
```

### `input/encoder.h` — реалізовано (Prompt 1)

```cpp
#pragma once
#include <stdint.h>

// Енкодер: апаратний PCNT (новий API), квадратурний режим на пінах A/B.
// Кнопка енкодера сюди не входить: це звичайна кнопка з модуля Buttons.
class Encoder {
public:
    // Налаштовує PCNT і створює задачу опитування. Потребує готового EventBus.
    // Повторний виклик безпечний (повертає true, друга задача не створюється).
    static bool begin();

    // Скільки подій енкодера відкинуто через переповнену чергу EventBus.
    static uint32_t droppedEvents();
};
```

### `input/ir_rc5.h` — заглушка (буде замінено в Prompt 2)

```cpp
#pragma once
#include <stdint.h>
struct IrRc5Code {
    uint8_t address;
    uint8_t command;
    bool toggle;
};
class IrRc5 {
public:
    static bool begin();
    static bool read(IrRc5Code& code);
    static void enterLearnMode();
};
```

### `ui/display.h`, `ui/fonts.h`, `ui/icons.h`, `ui/screens.h` — заглушки

```cpp
// display.h
#pragma once
class DisplayManager {
public:
    static bool begin();
    static void taskLoop();
};

// fonts.h
#pragma once
class UiFonts {
public:
    static bool begin();
};

// icons.h
#pragma once
#include <stdint.h>
enum class IconId : uint8_t {
    Wifi, Mute, Volume, Input, Standby,
};
class UiIcons {
public:
    static bool begin();
};

// screens.h
#pragma once
class UiScreens {
public:
    static bool begin();
};
```

### `net/wifi_manager.h`, `net/web_server.h`, `net/mdns.h` — заглушки

```cpp
// wifi_manager.h
#pragma once
class WifiManager {
public:
    static bool begin();
    static void poll();
};

// web_server.h
#pragma once
class WebServerManager {
public:
    static bool begin();
    static void poll();
};

// mdns.h
#pragma once
class MdnsManager {
public:
    static bool begin();
};
```

## 13. Константи `config/` (оновлювати вручну)

Повний вміст існуючих config-файлів. Їх не змінювати. Нові константи модулів — у `config/<module>_config.h`; після завершення модуля його константи (імена й значення) додаються сюди окремим підрозділом.

### `config/pins.h`

```cpp
#pragma once

// Єдина мапа пінів проекту.
// Джерело: MASTER_SPEC, розділ 4.
// Заборонені піни ESP32-S3: GPIO 26..37 (flash/PSRAM), 19/20 (USB).
// Не змінювати без прямої вказівки.

#include <stdint.h>

namespace pins {

// --- ST7789 (SPI2 / FSPI) ---
constexpr int kSt7789Sclk = 12;
constexpr int kSt7789Mosi = 11;
constexpr int kSt7789Cs   = 10;
constexpr int kSt7789Dc   = 9;
constexpr int kSt7789Rst  = 13;
constexpr int kSt7789Blk  = 14;   // LEDC PWM підсвітки

// --- I2S (PCM5102) ---
constexpr int kI2sBclk = 15;
constexpr int kI2sWs   = 17;      // LRCLK
constexpr int kI2sDout = 18;      // ESP32 -> DIN PCM5102
constexpr int kI2sMclk = 16;      // резерв PCM1808, НЕ драйвити
constexpr int kI2sDin  = 8;       // резерв PCM1808, не використовується

// --- Софт-мʼют PCM5102 ---
constexpr int kXsmt = 45;         // active-low, зовнішній pulldown 10 кОм

// --- I2C (TDA7318 / PT2313L) ---
constexpr int kI2cSda = 1;
constexpr int kI2cScl = 2;

// --- Енкодер ---
constexpr int kEncA   = 4;        // PCNT
constexpr int kEncB   = 5;        // PCNT
constexpr int kEncBtn = 6;

// --- Кнопки ---
constexpr int kBtnPower = 7;
constexpr int kBtnUp    = 21;
constexpr int kBtnDown  = 38;
constexpr int kBtnLeft  = 39;
constexpr int kBtnRight = 40;
constexpr int kBtnOk    = 41;

// --- IR приймач ---
constexpr int kIrIn = 47;         // RMT RX, active-low

// --- Резерв UART1 (майбутній BT-модуль) ---
constexpr int kUart1Tx = 42;
constexpr int kUart1Rx = 48;

// --- Консоль UART0 ---
constexpr int kUart0Tx = 43;
constexpr int kUart0Rx = 44;

}  // namespace pins
```

### `config/features.h`

```cpp
#pragma once

// Прапорці можливостей.
// Використовуються для умовної компіляції та заглушок.

// Апаратний АЦП для аналізатора рівня.
// У версії 1 не підключений.
#define ENABLE_PCM1808 0

// Майбутній Bluetooth-модуль через UART1.
// У версії 1 не використовується.
#define ENABLE_BT_UART 0

// VU-метр.
// Працює від декодованих PCM-семплів у режимі радіо.
// Архітектура має абстракцію джерела рівня (VuSource).
#define ENABLE_VU 1

// Обидва аудіопроцесори компілюються.
// Вибір конкретного зберігається в NVS і змінюється у веб-інтерфейсі.
// Автовизначення неможливе, бо адреса фіксована 0x44 і зчитування немає.
#define ENABLE_TDA7318 1
#define ENABLE_PT2313L 1

// Веб-інтерфейс і mDNS.
#define ENABLE_WEB 1
#define ENABLE_MDNS 1
```

### `config/defaults.h`

```cpp
#pragma once

// Значення за замовчуванням без «магічних» чисел у модулях.

#include <stdint.h>

namespace defaults {

// --- Консоль ---
constexpr uint32_t kSerialBaud = 115200;

// --- I2C ---
// Рівні: підтяжки до 3.3 В, швидкість 100 кГц.
// Процесор живиться від 9 В, пороги логічної одиниці звірити за даташитом.
constexpr uint32_t kI2cClockHz = 100000;
constexpr uint8_t kAudioProcessorI2cAddr = 0x44;

// --- Аудіопроцесор ---
// Діапазони уточнюються під конкретний чип у його драйвері.
constexpr int8_t kDefaultVolume  = 40;
constexpr int8_t kDefaultBass    = 0;
constexpr int8_t kDefaultTreble  = 0;
constexpr int8_t kDefaultBalance = 0;
constexpr bool   kDefaultMute    = false;
constexpr bool   kDefaultLoudness = false;

// Входи процесора:
// 0 = WiFi Radio, 1 = TV Box, 2 = Computer, 3 = Aux.
constexpr uint8_t kDefaultInput = 0;
constexpr uint8_t kInputCount = 4;
constexpr const char* kInputNames[kInputCount] = {
    "WiFi Radio",
    "TV Box",
    "Computer",
    "Aux",
};

// --- Дисплей ---
constexpr uint8_t kDefaultBrightness = 80;

// --- Подієва шина ---
constexpr uint16_t kEventQueueSize = 32;

// --- Wi-Fi provisioning ---
constexpr const char* kApSsid = "AudioCtrl-Setup";
constexpr uint8_t kWifiRetryBeforeAp = 3;

// --- mDNS ---
constexpr const char* kMdnsName = "audio";

// --- LittleFS ---
constexpr const char* kLittleFsMountPoint = "/littlefs";

}  // namespace defaults
```

### Константи модулів (`config/<module>_config.h`)

#### `config/input_config.h` — реалізовано (Prompt 1)

```cpp
#pragma once

// Константи модулів input/buttons та input/encoder.
// Усі часові значення — у мілісекундах, якщо не вказано інше.
// Піни тут не дублюються: вони лише в config/pins.h.

#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог кожної події кнопок і енкодера: "[BTN] UP short", "[ENC] CW delta=2".
#define INPUT_DEBUG 1

// Тимчасовий вивід усіх подій з EventBus у main.cpp ("[EVT] ...").
// Вимкнути, коли з'явиться AppController, який сам читає EventBus.
#define INPUT_DEMO_PRINT_EVENTS 1

namespace input_cfg {

// ---------------------------------------------------------------------------
// Задачі FreeRTOS
// ---------------------------------------------------------------------------
// Розподіл ядер проекту: ядро 0 = UI, мережа, введення, AppController, веб;
// ядро 1 = аудіо. Введення працює на ядрі 0.
//
// Пріоритети: введення НИЖЧЕ за аудіо, але ВИЩЕ за UI. Тобто пріоритет аудіо-
// задачі має бути > kBtnTaskPriority / kEncTaskPriority, а пріоритет UI-задач
// (AppController, дисплей) — нижчий за них. Задачі введення майже весь час
// сплять (vTaskDelayUntil), тож витісняти UI вони будуть на мікросекунди.
constexpr int      kBtnTaskCore       = 0;
constexpr uint8_t  kBtnTaskPriority   = 4;
constexpr uint32_t kBtnTaskStackBytes = 4096;

constexpr int      kEncTaskCore       = 0;
constexpr uint8_t  kEncTaskPriority   = 4;
constexpr uint32_t kEncTaskStackBytes = 4096;

// ---------------------------------------------------------------------------
// Кнопки
// ---------------------------------------------------------------------------
// Активний рівень: 0 = LOW (кнопка замикає пін на GND, внутрішня підтяжка).
constexpr uint8_t  kBtnPressedLevel   = 0;

// Пауза після pinMode(INPUT_PULLUP), щоб підтяжка встигла встановити рівень.
constexpr uint32_t kBtnPullupSettleMs = 5;

// Період опитування кнопок.
constexpr uint32_t kBtnPollMs         = 5;

// Дебаунс: новий стан приймається, якщо він стабільний не менше цього часу.
constexpr uint32_t kBtnDebounceMs     = 30;

// Довге натискання: подія при утриманні не менше цього часу.
constexpr uint32_t kBtnLongPressMs    = 800;

// Повтор (лише UP/DOWN/LEFT/RIGHT): початкова затримка й період.
constexpr uint32_t kBtnRepeatDelayMs  = 500;
constexpr uint32_t kBtnRepeatPeriodMs = 120;

// Довге натискання для кожної кнопки окремо.
// Якщо вимкнено — кнопка дає лише коротку подію при відпусканні
// (незалежно від тривалості утримання).
constexpr bool kBtnLongPressPower  = true;
constexpr bool kBtnLongPressUp     = false;
constexpr bool kBtnLongPressDown   = false;
constexpr bool kBtnLongPressLeft   = false;
constexpr bool kBtnLongPressRight  = false;
constexpr bool kBtnLongPressOk     = true;
constexpr bool kBtnLongPressEncBtn = true;

// ---------------------------------------------------------------------------
// Перевірка утримання кнопки при старті (Buttons::isHeldAtBoot)
// ---------------------------------------------------------------------------
// Період опитування піна під час перевірки.
constexpr uint32_t kBootSamplePeriodMs  = 10;

// Скільки утримувати OK при старті для скидання Wi-Fi.
constexpr uint32_t kBootWifiResetHoldMs = 3000;

// ---------------------------------------------------------------------------
// Енкодер (PCNT, квадратурний режим)
// ---------------------------------------------------------------------------
// Межі апаратного лічильника. Для акумуляції переповнень драйвер PCNT
// рекомендує якомога більші межі (менше переривань). Максимум 16 біт.
constexpr int kEncPcntLowLimit  = -30000;
constexpr int kEncPcntHighLimit = 30000;

// Glitch filter PCNT, нс. Фільтрує лише електричні голки (апаратний максимум
// ~12 мкс при APB 80 МГц). Дребезг контактів гаситься самим квадратурним
// декодуванням (+1 і -1 взаємно знищуються). 0 = фільтр вимкнено.
constexpr uint32_t kEncGlitchNs = 5000;

// Скільки імпульсів PCNT (повний квадратурний x4) припадає на один detent.
// Типові EC11: 4 (частіше) або 2.
constexpr int  kEncCountsPerDetent = 4;

// Інверсія напрямку: якщо CW дає CCW — поставити true.
constexpr bool kEncInvertDirection = false;

// Період опитування лічильника PCNT.
constexpr uint32_t kEncPollMs = 10;

// Акселерація: якщо між двома подіями обертання в той самий бік минуло менше
// kEncAccelThresholdMs, delta множиться на kEncAccelMultiplier.
constexpr bool     kEncAccelEnabled     = true;
constexpr uint32_t kEncAccelThresholdMs = 50;
constexpr uint8_t  kEncAccelMultiplier  = 3;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kBtnPollMs > 0, "kBtnPollMs must be > 0");
static_assert(kBtnDebounceMs >= kBtnPollMs, "debounce must be >= poll period");
static_assert(kBtnRepeatPeriodMs >= kBtnPollMs, "repeat period must be >= poll period");
static_assert(kBootSamplePeriodMs > 0, "kBootSamplePeriodMs must be > 0");
static_assert(kEncPcntLowLimit < 0 && kEncPcntHighLimit > 0, "PCNT limits must straddle zero");
static_assert(kEncPcntLowLimit >= -32767 && kEncPcntHighLimit <= 32767, "PCNT limits are 16-bit");
static_assert(kEncCountsPerDetent > 0, "kEncCountsPerDetent must be > 0");
static_assert(kEncPollMs > 0, "kEncPollMs must be > 0");
static_assert(kEncAccelMultiplier >= 1, "kEncAccelMultiplier must be >= 1");

}  // namespace input_cfg
```

_(наступний модуль додасть сюди `config/ir_config.h` після Prompt 2)_