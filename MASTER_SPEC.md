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
- **Аудіопроцесор:** TDA7318 АБО PT2313L (I2C, адреса 0x44, зчитування немає, автовизначення неможливе). Вибір типу зберігається в NVS, змінюється у веб-інтерфейсі. **Кількість фізичних входів різна:** TDA7318 — 4, PT2313L — 3. У проекті 4 логічних входи (WiFi Radio/TV Box/Computer/Aux); якщо обрано PT2313L, логічний вхід 3 (Aux) недоступний — UI/веб ховають пункти меню входів понад `capabilities().inputCount` (рішення зафіксовано, варіант «а» з обговорення перед Prompt 4).
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
| `audio/` | `audio_player` (ESP32-audioI2S), `audio_processor.h` (інтерфейс), `audio_i2c` (спільна I2C-шина, мʼютекс, кеш регістрів), `tda7318`, `pt2313l`, `vu_source`, `audio_proc_test` (тестовий Serial-режим, лише `AUDIO_PROC_TEST`) |
| `stations/` | `station_store` (LittleFS JSON), імпорт M3U / PLS / JSON |
| `ui/` | `display` (LovyanGFX), `fonts` (укр.), `icons`, `screens` |
| `net/` | `wifi_manager` (STA + AP provisioning + captive portal), `web_server`, `mdns` |
| корінь `src/` | `main.cpp` |

## 7. Інтерфейс аудіопроцесора

Спільний абстрактний інтерфейс для TDA7318 і PT2313L (актуальний код — розділ 12):

- `begin()`, `setInput(index)`, `setVolume(value)`, `setBass(value)`, `setTreble(value)`, `setBalance(value)`, `setMute(bool)`, `setLoudness(bool)`, `probe()` (ACK на I2C-адресу, стан не змінює), `applyAll()` (повторно надіслати весь закешований стан — спершу мʼют гучномовців окремою транзакцією, потім решта, потім повернути баланс), `i2cErrorCount()`, `cachedState()`.
- `capabilities()` — реальні можливості чипа: `bass`, `treble`, `balance`, `loudness`, `inputCount` (TDA7318 = 4, PT2313L = 3 — див. розділ 3), `volumeMin/Max`, `toneMin/Max`, `balanceMin/Max` (усе — шкала UI, не сирі регістри), `fader` (апаратно є в обох чипах, але інтерфейс не дає `setFader()`, тому завжди `false`), `inputGain` (підсилення конкретного входу, якщо драйвер це реалізує).
- UI/AppController ховають непідтримувані функції та пункти входів понад `inputCount`.
- Плавна зміна гучності (ramp) — відповідальність ВИКЛИКАЧА (`AppController`): він сам покроково викликає `setVolume()`; драйвер не блокує і не має власного таймера. Кожен виклик — одна коротка I2C-транзакція; повторні однакові значення в шину не йдуть (кеш `audio_i2c::RegShadow`).
- Мʼют = мʼют усіх атенюаторів гучномовців (регістр гучності не чіпається). Послідовність «мʼют → зміна входу/станції → розмʼют» координує `AppController`; драйвер надає лише окремі примітиви.
- Спільна I2C-шина (`audio/audio_i2c`): один статичний рекурсивний мʼютекс на обидва драйвери й на тестовий режим, кеш підтверджених байтів на регістр, таймаут і повтори транзакцій — константи в `config/audio_config.h`.
- Рівні I2C: підтяжки до 3.3 В, швидкість 100 кГц; при живленні процесора 9 В пороги логічної одиниці звірені за даташитом (винесено в реалізацію драйверів).

## 8. Wi-Fi provisioning

- Немає збереженої мережі або 3 невдалих спроби підключення → режим AP з назвою `AudioCtrl-Setup`, captive portal (DNS), сторінка: скан мереж, вибір, пароль → збереження в NVS → перезапуск у режим STA.
- На дисплеї в цей час — інструкція (назва AP, адреса).
- Скидання Wi-Fi — утримання кнопки OK під час старту.
- Та сама веб-частина, що й для налаштувань: після підключення до мережі доступний повний інтерфейс за адресою mDNS (`audio.local`) і IP.

## 9. Веб-інтерфейс (перелік можливостей)

Стан (`/api/status`), налаштування (тип процесора, назви входів, яскравість, Wi-Fi), станції (перегляд, редагування, імпорт JSON/M3U/PLS, експорт), навчання кнопок IR, OTA-оновлення прошивки. Сторінки зберігаються в LittleFS (стиснуті gzip).

## 10. Українська мова та іконки на дисплеї

- Повний український алфавіт, включно з `Є є І і Ї ї Ґ ґ`. Вбудовані шрифти LovyanGFX (`efontJA` тощо) НЕ мають потрібних літер (щонайменше `Ґ Є І Ї`). Рішення (Prompt 4): власний растровий шрифт `GFXfont` на основі DejaVu Sans, генерується інструментом `tools/gen_gfxfont.py` у файл `ui/font_data.h`, покриває ASCII/Latin-1/Latin Extended-A/кирилицю U+0400–U+045F і Ґ/ґ (U+0490/0491). **Перевірити:** що `tools/gen_gfxfont.py` і згенерований `ui/font_data.h` справді є в репозиторії (без них `fonts.cpp` не збереться), і візуально звірити рядок з проблемними літерами на реальному екрані — коментар коду про це не є доказом.
- Типографські символи (тире, лапки, багатокрапка) у згенерованій таблиці відсутні: перед виведенням їх треба нормалізувати в ASCII-аналоги (майбутній модуль обробки ICY-метаданих).
- Метадані ICY можуть приходити в UTF-8, cp1251 або Latin-1: потрібне визначення кодування й конвертація в UTF-8.
- Довгі назви станцій і заголовки — прокрутка (marquee) без мерехтіння; `UiFonts::textWidth()` дає ширину рядка в пікселях для цього.
- Іконки — 1-бітні маски `kIconSize × kIconSize` (24×24), колір задає викликач; малює їх виключно `DisplayManager::drawIcon()` (єдиний власник спрайту), `UiIcons` лише віддає маску й назву.
- Розкладка екрана (VU, назва станції, метадані) підбирається експериментально: усі координати й розміри виносити в константи (див. `display_cfg::demo::*` як приклад для тестового кадру; реальні екрани матимуть власний набір).

## 11. Поточний стан проекту (оновлювати вручну)

**Тулчейн:** pioarduino 55.03.37 (Arduino-ESP32 3.3.7 / IDF 5.5.2), board `esp32-s3-devkitc-1`, `qio_opi`, консоль UART0, USB CDC вимкнено.

**Бібліотеки:** LovyanGFX ^1.2.0, ESP32-audioI2S (git), ESPAsyncWebServer + AsyncTCP (ESP32Async, git), ArduinoJson ^7.4.1. `lib_ldf_mode = chain`.

**Виконані модулі:**
- Скелет проекту (platformio.ini, partitions.csv, config/*, заглушки модулів, main.cpp).
- `core/events.h` (`EventBus`) — реалізовано.
- `input/buttons`, `input/encoder`, `config/input_config.h` — реалізовано (Prompt 1).
- `input/ir_rc5`, `config/ir_config.h` — реалізовано (Prompt 2). Інструкція користувача — `IR_LEARNING.txt`.
- `audio/audio_processor.h` (розширено), `audio/tda7318`, `audio/pt2313l`, `audio/audio_i2c`, `audio/audio_proc_test`, `config/audio_config.h` — реалізовано (Prompt 3).
- `ui/display`, `ui/fonts`, `ui/icons`, `config/display_config.h` — реалізовано (Prompt 4): ST7789 через LovyanGFX, подвійний буфер-спрайт у PSRAM, задача малювання, власний український шрифт (`ui/font_data.h`, генерується `tools/gen_gfxfont.py`), 1-бітні іконки, статичний тестовий кадр (`DISPLAY_DEMO`).

**Важливо для AppController (ще не написаний), врахувати при реалізації:**
- `Action::ENC_PRESS` з `longPress=false` → mute; `longPress=true` → інша дія. POWER і OK також мають окремі довгі події.
- Вимкнути `INPUT_DEMO_PRINT_EVENTS` у `input_config.h`, коли AppController підключить власне читання `EventBus`.
- `Buttons::droppedEvents()` / `Encoder::droppedEvents()` / `IrRc5::droppedEvents()` — лічильники втрат подій.
- IR: `ENC_CW`/`ENC_CCW` з `source==IR` завжди `delta=1`; `repeat=true` від IR лише для VOL_UP, VOL_DOWN, UP, DOWN, LEFT, RIGHT.
- Режим навчання IR поки керується тимчасовим кодом у `main.cpp` (`IR_LEARNING.txt`); коли зʼявиться екран `IrLearn`, він викликає ту саму статичну API `IrRc5`.
- **Вибір типу процесора (TDA7318/PT2313L) зберігається в `Settings` (NVS), `AppController` створює драйвер один раз через `createAudioProcessor()`.**
- `audio_cfg::kTestProcType` — НЕ рішення проєкту, а відображення поточного макетного стенду (зараз на макетці розпаяно PT2313L; на фінальній платі буде TDA7318). Значення константи поміняти на `Tda7318`, коли тестування переїде на фінальне залізо; коли зʼявиться `Settings`, ця константа взагалі йде в архів (тип читається з NVS).
- **PT2313L має лише 3 фізичні входи (TDA7318 — 4).** Логічний вхід 3 (Aux) недоступний, коли активний PT2313L: `AppController`/UI мають ховати/пропускати пункти меню входів з індексом `>= capabilities().inputCount`. Узгоджене рішення, не переробляти без потреби.
- Ramp гучності — відповідальність `AppController`: він сам кроково викликає `setVolume()`; драйвер не блокує.
- Послідовність при зміні входу/станції: `setMute(true)` → зміна → `setMute(false)`, реалізує `AppController`.
- Після відновлення зі standby або підозри на збій I2C — викликати `applyAll()`.
- Serial-тестовий режим аудіопроцесора (`AUDIO_PROC_TEST`) вимкнути, коли `AppController` візьме керування процесором на себе.
- **Орієнтація дисплея (`DisplayManager::setFlipped()`) поки НЕ зберігається** — при старті завжди `display_cfg::kDefaultFlipped`. Коли зʼявиться `Settings`, додати туди поле (наприклад `displayFlipped`) і викликати `setFlipped()` при старті з цього значення; веб-інтерфейс дає можливість перемкнути.
- `DisplayManager` не читає `EventBus` і не знає про екрани — реальний контент задає `FrameCallback` (`ui/screens`, ще не написаний), що викликається з display-задачі кожен період кадру.
- Примітиви малювання `DisplayManager` (`fillScreen`, `drawText`, `drawIcon` тощо) і `present()` НЕ потокобезпечні — викликати лише з display-задачі (через `FrameCallback`), інші задачі — лише `setBrightness()`/`setFlipped()`/гетери.
- Вимкнути `DISPLAY_DEMO`, коли з'явиться `ui/screens` і реальний `FrameCallback`.

**Заплановані зміни інтерфейсів (виконуються у відповідних промптах):**
- `AppState`: додати безпечне часткове оновлення (`modify`) при реалізації AppState.
- `Settings`: додати `bass`, `treble`, `balance`, `loudness`, `processorType`, `displayFlipped`, `brightness` (уже є в структурі-заглушці, перевір узгодженість типу).

**Відомі особливості реалізації (не проблеми, але важливо знати):**
- PCNT: поле `accum_count` у `pcnt_unit_config_t` — без `flags.` у IDF 5.5.2. Якщо збірка видасть `no member named 'flags'` — прибрати `flags.`.
- Дебаунс кнопок: коротка подія — приблизно через `kBtnDebounceMs` (30 мс) після відпускання; довге — приблизно через `kBtnLongPressMs` (800 мс) від початку натискання.
- `enum class Action` зберігається в NVS як числа (мапа IR) — нові дії лише в кінець `enum`.
- IR: несуча пульта 36 кГц проти розрахункових 38 кГц VS1838B — підбирається `ir_cfg::kMarkBiasUs`/`kHalfBitTolerancePercent`, не перевірено на залізі. Стартова мапа порожня. Розширений RC5X (20 біт) не підтримується.
- Аудіопроцесор: обидва драйвери використовують спільний формат байтів (родина TDA7313/7318/PT2313), різняться кроком підсилення входу (6.25 дБ проти 3.75 дБ), наявністю loudness (PT2313L — лише 28-pin) і кількістю фізичних входів (4 проти 3). `fader` завжди `false` (немає окремого `setFader()` в інтерфейсі, хоча апаратно можливий). `kPowerOnSettleMs` (300 мс) варто звірити з даташитом конкретного номіналу Cref PT2313L.
- **Дисплей: `kOffsetX/Y`, `kInvertColors`, `kBgrOrder` — ПРИПУЩЕННЯ, НЕ перевірені на реальному залізі** (значення підібрані як типові для панелей 170×320, з чіткими ознаками помилки в коментарях `display_config.h`: зсунуті кути → offset, чорний виглядає білим → invert, червоний кут виглядає синім → BGR). Перевірити тестовим кадром (`DISPLAY_DEMO`) при першому включенні.
- Дисплей: SPI на 80 МГц без DMA (`kUseDma=false`) — якщо зʼявляться артефакти на макетних дротах, спершу знизити `kSpiWriteHz` до 40 МГц, а не одразу підозрювати офсет/колір.
- Дисплей: подвійний буфер (2 × ~108 КБ у PSRAM) обраний для плавності VU-метра; якщо PSRAM забракне під час додавання VU/аудіобуферів — можна повернутись до одного буфера ціною можливого мерехтіння.

**Відомі проблеми:** поки немає.

## 12. Інтерфейси готових модулів (оновлювати вручну)

Нижче лише публічні інтерфейси: реалізації опущено.

### `core/events.h` — реалізовано (розширено в Prompt 2)

```cpp
#pragma once

// Подієва шина.
// Єдина FreeRTOS-черга подій.
// Джерела: кнопки, енкодер, IR, веб.
// Модулі не викликають один одного напряму для керування.

#include <Arduino.h>
#include <stdint.h>

// Логічні дії.
// Нові значення додавати ЛИШЕ в кінець: числові значення Action зберігаються в NVS (мапа IR).
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
    // [Prompt 2] ДОДАНО: VOL_UP..BACK, INPUT_RADIO..INPUT_AUX, DIGIT_0..DIGIT_9 (цифри йдуть підряд).
    VOL_UP,
    VOL_DOWN,
    MUTE,
    MENU,
    BACK,
    INPUT_RADIO,
    INPUT_TV,
    INPUT_PC,
    INPUT_AUX,
    DIGIT_0,
    DIGIT_1,
    DIGIT_2,
    DIGIT_3,
    DIGIT_4,
    DIGIT_5,
    DIGIT_6,
    DIGIT_7,
    DIGIT_8,
    DIGIT_9,
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

### `audio/audio_processor.h` — реалізовано (розширено в Prompt 3)

```cpp
#pragma once
#include <stdint.h>

// Абстрактний інтерфейс аудіопроцесора (TDA7318 / PT2313L).
// [Prompt 3] Розширено: AudioProcType, probe(), applyAll(), i2cErrorCount(),
// cachedState(), поля fader/inputGain/balanceMin/balanceMax у capabilities,
// фабрика createAudioProcessor(). Наявні методи не змінювалися.

// Тип чіпа. Автовизначення неможливе (обидва на 0x44, зчитування немає),
// тому тип задається ззовні (згодом — з NVS/Settings: 0 = Tda7318, 1 = Pt2313l).
enum class AudioProcType : uint8_t {
    Tda7318,
    Pt2313l,
};

struct AudioProcessorCapabilities {
    bool bass;
    bool treble;
    bool balance;
    bool loudness;
    uint8_t inputCount;
    // Діапазони нижче — це шкала UI, а не сирі значення регістрів:
    //   volume  — 0..100 (крива в audio_cfg::volumeAttSteps),
    //   tone    — кроки по 2 дБ (audio_cfg::kToneStepDb),
    //   balance — кроки по 1.25 дБ; знак «+» = правий канал гучніший.
    int8_t volumeMin;
    int8_t volumeMax;
    int8_t toneMin;
    int8_t toneMax;
    // [Prompt 3] ДОДАНО
    // Окремий перед/зад: true лише якщо його можна керувати через цей інтерфейс.
    // Обидва чіпи мають 4 незалежні атенюатори (апаратно фейдер можливий),
    // але setFader() в інтерфейсі немає, тому обидва драйвери повертають false.
    bool fader;
    // Підсилення конкретного входу (задається в audio_cfg::kInputGainSteps).
    bool inputGain;
    int8_t balanceMin;
    int8_t balanceMax;
};

// [Prompt 3] ДОДАНО: останній ЗАПИТАНИЙ стан (не те, що гарантовано лежить у чіпі
// після збою шини — для цього є applyAll()).
struct AudioProcessorState {
    uint8_t input;
    int8_t volume;
    int8_t bass;
    int8_t treble;
    int8_t balance;
    bool mute;
    bool loudness;
};

// Усі методи потокобезпечні (спільний мʼютекс I2C-шини).
// Методи setXxx() повертають true, якщо команда виконана або не потрібна
// (значення вже у чіпі); false — шина не відповіла, непідтримувана функція
// або некоректний індекс входу. Значення поза діапазоном обрізаються до меж.
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

    // [Prompt 3] ДОДАНО
    // Чи відповідає щось на I2C-адресу процесора (ACK). Стан не змінює.
    virtual bool probe() = 0;
    // Повторно надіслати весь закешований стан (після standby / збою шини).
    virtual bool applyAll() = 0;
    // Скільки I2C-операцій запису не вдалося навіть після повторів (діагностика).
    virtual uint32_t i2cErrorCount() const = 0;
    // Останній запитаний стан.
    virtual AudioProcessorState cachedState() const = 0;
};

// [Prompt 3] ДОДАНО: фабрика. Обʼєкт створюється через new (не PSRAM) і живе
// весь час роботи прошивки. nullptr — тип вимкнено в config/features.h
// (ENABLE_TDA7318 / ENABLE_PT2313L) або не вистачило памʼяті.
AudioProcessor* createAudioProcessor(AudioProcType type);
```

### `audio/audio_i2c.h` — реалізовано (Prompt 3)

```cpp
#pragma once

// Спільна I2C-шина аудіопроцесорів (TDA7318 / PT2313L).
// Один статичний рекурсивний мʼютекс на обидва драйвери: якщо їх колись
// створять по черзі, вони не заважатимуть один одному, а шину не смикатимуть
// одночасно AppController і тестовий режим.
//
// Драйвери беруть Lock на весь метод (стан + кеш + запис), а write()/probe()
// беруть його ще раз (рекурсивно) — тому без взаємного блокування.

#include <stddef.h>
#include <stdint.h>

namespace audio_i2c {

// Створює мʼютекс і ініціалізує Wire (піни/швидкість/таймаут з config/).
// Ідемпотентна. Викликати з setup(), не з кількох задач одночасно.
bool begin();

bool isReady();

// Порожня транзакція на адресу процесора: true, якщо ACK. Стан не змінює.
bool probe();

// Одна транзакція запису (адреса + len байтів даних) з повторами.
// true — усі байти підтверджено. Помилку логує з обмеженням частоти.
bool write(const uint8_t* data, size_t len);

// RAII-захоплення мʼютекса шини (рекурсивне). ok() == false — не вдалося
// захопити за kI2cMutexTimeoutMs (або begin() ще не викликано).
class Lock {
public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    bool ok() const { return m_taken; }

private:
    bool m_taken;
};

// Кеш останніх байтів, ПІДТВЕРДЖЕНИХ чипом, по одному на регістр (до 8).
// Запис пропускається, якщо ті самі байти вже лежать у чипі. Після невдалого
// запису запис кешу скидається, тож наступний виклик з тим самим значенням
// таки піде в шину.
class RegShadow {
public:
    static constexpr uint8_t kMaxRegs = 8;

    void invalidateAll() { m_valid = 0; }

    void invalidate(uint8_t idx) {
        m_valid = static_cast<uint8_t>(m_valid & ~(1u << idx));
    }

    bool isSame(uint8_t idx, uint8_t value) const {
        return ((m_valid >> idx) & 1u) != 0 && m_value[idx] == value;
    }

    void set(uint8_t idx, uint8_t value) {
        m_value[idx] = value;
        m_valid = static_cast<uint8_t>(m_valid | (1u << idx));
    }

private:
    uint8_t m_value[kMaxRegs] = {};
    uint8_t m_valid = 0;
};

}  // namespace audio_i2c
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

### `audio/tda7318.h` та `audio/pt2313l.h` — реалізовано (Prompt 3), стиснено

Обидва — `class Tda7318 : public AudioProcessor` і `class Pt2313l : public AudioProcessor`, публічний інтерфейс ІДЕНТИЧНИЙ базовому `AudioProcessor` (розділ вище) без додаткових публічних методів. Приватні деталі (регістри, кеш, мʼютекс-обгортки) нікому, крім самих драйверів, не потрібні — прибрано зі спеку заради розміру; повний код лишається в репозиторії. Ключове, що варто памʼятати про поведінку (без коду):
- **Tda7318:** 4 фізичні входи, loudness відсутній, ramp гучності й послідовність мʼюту — відповідальність викликача (див. розділ 11).
- **Pt2313l:** 3 фізичні входи (`setInput(3)` → `false`), loudness лише за `audio_cfg::kPt2313lHasToneLoudness` (28-pin корпус), формат байтів як у Tda7318, крім перемикача (біт 2 = loudness, 0 = увімкнено) і кроку підсилення входу (3.75 дБ проти 6.25 дБ).
- Обидва: мʼют = мʼют усіх атенюаторів гучномовців (регістр гучності не чіпається), кеш регістрів через `audio_i2c::RegShadow`, лічильник помилок і стан — через `i2cErrorCount()`/`cachedState()` з базового інтерфейсу.

### `audio/audio_proc_test.h` — реалізовано (Prompt 3)

```cpp
#pragma once

// Тестовий режим аудіопроцесора: Serial-команди для ручної перевірки без решти проекту.
// Компілюється лише при AUDIO_PROC_TEST == 1 (config/audio_config.h).
// Протокол команд — у audio_proc_test.cpp (команда `h` друкує його в Serial).

#include "config/audio_config.h"

#if AUDIO_PROC_TEST

class AudioProcessor;

class AudioProcTest {
public:
    // Запускає задачу читання Serial (ядро 0, пріоритет 1). Повторний виклик безпечний.
    // proc має жити весь час роботи (його створює createAudioProcessor).
    static bool begin(AudioProcessor* proc);
};

#endif  // AUDIO_PROC_TEST
```

Протокол команд `AudioProcTest` (Serial, команда + Enter, регістр байдужий): `i<N>` вхід, `v<0..100>` гучність, `b<-7..7>` бас, `t<-7..7>` дискант, `a<-20..20>` баланс (+ = правий гучніший), `m[0|1]` мʼют, `l[0|1]` loudness, `p` — capabilities і кеш, `r` — `applyAll()`, `x` — `probe()`, `h` — допомога. Відповідь у форматі `[ATEST] volume 50 -> OK|FAIL`.

### `audio/audio_player.h` — заглушка (буде реалізовано в Prompt 5)

```cpp
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

### `input/ir_rc5.h` — реалізовано (Prompt 2)

```cpp
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "core/events.h"

// Результат декодування одного кадру RC5/RC5X.
struct IrRc5Frame {
    uint8_t addr;          // 0..31
    uint8_t cmd;           // 0..127 (для RC5X 7-й біт вже інвертовано з S2)
    bool toggle;
    bool isRC5X;           // true, якщо S2 == 0 (тоді cmd >= 64)
    uint32_t timestampMs;  // millis() на момент обробки кадру
};

// IR-приймач RC5/RC5X: RMT RX, мапа кодів у NVS, режим навчання.
// Усі методи, крім begin(), викликати після begin().
class IrRc5 {
public:
    enum class LearnStatus : uint8_t {
        Idle,      // навчання не активне
        Waiting,   // чекаємо перше натискання
        Confirm,   // перший код прийнято, чекаємо такого ж другого натискання
        Success,   // код привʼязано й збережено
        Timeout,   // вичерпано час очікування
        Conflict,  // код уже привʼязаний до іншої дії; чекаємо confirmOverwrite()
    };

    // Створює задачу приймання, налаштовує RMT RX, завантажує мапу з NVS.
    // Потребує готового EventBus. Повторний виклик безпечний (повертає true).
    static bool begin();

    // Скільки подій відкинуто через переповнену чергу EventBus.
    static uint32_t droppedEvents();

    // Скільки прийнятих кадрів відкинуто як невалідні (діагностика шуму).
    static uint32_t decodeErrors();

    // --- Мапа кодів (потокобезпечна) ---

    // Завантажує мапу з NVS. true — прочитано валідну мапу (можливо, порожню).
    // false — нічого не збережено або дані пошкоджені (мапа очищена).
    static bool load();

    // Зберігає мапу в NVS.
    static bool save();

    // Очищає мапу в памʼяті й у NVS.
    static void clearMap();

    // Видаляє всі коди дії й зберігає. true, якщо щось було видалено.
    static bool removeAction(Action action);

    // Дія за кодом (addr, cmd).
    static bool lookup(uint8_t addr, uint8_t cmd, Action& out);

    // Перший код, привʼязаний до дії.
    static bool codeFor(Action action, uint8_t& addr, uint8_t& cmd);

    // Кількість записів у мапі.
    static size_t mapSize();

    // --- Навчання ---

    // Починає навчання для дії. Звичайна генерація подій призупиняється.
    // false — дія некоректна або мапа заповнена (і в дії немає власного запису).
    static bool beginLearn(Action action);

    // Завершує навчання будь-якого стану (у т.ч. скидає Success/Timeout в Idle).
    static void cancelLearn();

    // Поточний стан. Success/Timeout лишаються, доки не буде beginLearn/cancelLearn.
    static LearnStatus status();

    // Підтвердити перепризначення в стані Conflict (старий привʼязаний до
    // цього коду запис іншої дії видаляється). true — статус став Success.
    static bool confirmOverwrite();

    // Для UI: дія, яку навчають.
    static Action learnTarget();

    // Для UI: кандидат (доступний у Confirm/Conflict).
    static bool learnCandidate(uint8_t& addr, uint8_t& cmd);

    // Для UI: з якою дією конфлікт (доступно лише в Conflict).
    static bool learnConflictWith(Action& other);

    // --- JSON (для майбутнього веб-інтерфейсу) ---

    // Записує мапу як масив: [{"action":"VOL_UP","addr":0,"cmd":16,"rc5x":false}, ...]
    // false — документ переповнено.
    static bool exportJson(JsonDocument& doc);

    // Замінює мапу вмістом масиву й зберігає в NVS. Атомарно: при будь-якій
    // помилці валідації мапа не змінюється. false також під час навчання
    // або якщо не вдалося зберегти в NVS (тоді мапа в RAM уже оновлена).
    static bool importJson(const JsonDocument& doc);
};
```

### `ui/display.h` — реалізовано (Prompt 4)

```cpp
#pragma once

// DisplayManager: ST7789 170×320 (у проєкті горизонтально, 320×170) через LovyanGFX.
//
// Архітектура:
//  - малювання лише у власні повнокадрові спрайти RGB565 у PSRAM (2 буфери);
//  - весь цикл кадру виконується в ОДНІЙ задачі FreeRTOS (taskLoop);
//  - примітиви малюють у «задній» буфер, present() виводить його на панель і
//    міняє буфери місцями. Після present() вміст нового заднього буфера
//    НЕВИЗНАЧЕНИЙ: кадр малюється повністю (починаючи з fillScreen).
//
// ПОТОКОВА МОДЕЛЬ: примітиви малювання й present() НЕ потокобезпечні. Їх слід
// викликати лише з display-задачі (з FrameCallback або з taskLoop). Інші
// задачі можуть викликати лише setBrightness()/brightness()/isReady()/
// lastError*()/setFlipped()/isFlipped(). Реальний контент (екрани) зʼявиться
// через FrameCallback.
//
// DisplayManager не читає EventBus і не знає про екрани.

#include <stdint.h>

#include "config/display_config.h"
#include "ui/fonts.h"
#include "ui/icons.h"

// Результат останньої спроби begin().
enum class DisplayError : uint8_t {
    None,                // усе гаразд
    NotStarted,          // begin() ще не викликали
    NoPsram,             // PSRAM не знайдено
    ResourceInitFailed,  // не ініціалізовано шрифти/іконки
    BacklightInitFailed, // ledcAttach() для BLK не вдався
    PanelInitFailed,     // init() панелі не вдався або розмір ≠ 320×170
    SpriteAllocFailed,   // не вдалося виділити буфери кадру в PSRAM
    TaskCreateFailed,    // не створено задачу малювання
};

class DisplayManager {
public:
    // Викликається кожен період кадру з display-задачі. Малює кадр примітивами
    // нижче й повертає true, якщо кадр треба вивести (present() викличе задача).
    // Повертає false — панель лишається як була. Встановлюється до або після begin().
    using FrameCallback = bool (*)();

    // Ініціалізація: PSRAM-перевірка, шрифти, іконки, підсвітка (вимкнена), панель
    // (апаратний скид через RST), буфери, задача. Повторний виклик після успіху
    // повертає true. Підсвітка вмикається після виводу ПЕРШОГО кадру.
    static bool begin();

    // Тіло задачі малювання. Не викликати вручну: його запускає begin().
    static void taskLoop();

    static DisplayError lastError();
    static const char* lastErrorName();
    static bool isReady();

    // Підсвітка, 0..100 %. Безпечно з будь-якої задачі. До першого кадру значення
    // лише запамʼятовується. Лінійна залежність яскравості від шпаруватості.
    static void setBrightness(uint8_t percent);
    static uint8_t brightness();

    static void setFrameCallback(FrameCallback callback);

    // Орієнтація: false = kRotationNormal, true = розвернуто на 180°
    // (kRotationFlipped). Безпечно з будь-якої задачі й до begin(): запит лише
    // запамʼятовується, а поворот панелі виконує display-задача в наступному
    // періоді кадру й одразу повторно виводить останній кадр. Розмір 320×170
    // не змінюється, координати екранів переписувати не треба.
    static void setFlipped(bool flipped);
    static bool isFlipped();

    static int16_t width();
    static int16_t height();

    // --- Примітиви (лише з display-задачі; координати в пікселях, 0,0 = лівий верх) ---
    static void fillScreen(uint16_t color);
    static void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    static void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    static void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

    // Текст UTF-8, (x, y) — лівий верхній кут. Перша версія — прозоре тло,
    // друга — тло кольору bg (стирає попередній напис у межах символів).
    static void drawText(const char* utf8, int16_t x, int16_t y, FontSize size, uint16_t fg);
    static void drawText(const char* utf8, int16_t x, int16_t y, FontSize size, uint16_t fg,
                         uint16_t bg);

    // Іконка kIconSize×kIconSize, (x, y) — лівий верхній кут. Вся робота зі
    // спрайтом (drawBitmap) — тут; UiIcons дає лише маску.
    static void drawIcon(IconId id, int16_t x, int16_t y,
                         uint16_t color = display_cfg::kColorFg);

    // Вивести готовий задній буфер на панель і поміняти буфери місцями.
    static void present();
};
```

### `ui/fonts.h` — реалізовано (Prompt 4)

```cpp
#pragma once

// Українські шрифти для дисплея.
//
// Власні растрові шрифти GFXfont (1 біт/піксель) з DejaVu Sans, згенеровані
// tools/gen_gfxfont.py у ui/font_data.h. Покриття: ASCII, Latin-1, Latin
// Extended-A, уся кирилиця U+0400..U+045F і Ґ ґ (U+0490/0491) — тобто повний
// український алфавіт. Вбудовані шрифти LovyanGFX (efontJA) НЕ мають
// Ґ Є І Ї (підтверджено на залізі), тому не використовуються.
// Типографські символи (— – " " ' …) у таблицю не входять: перед
// виведенням їх треба замінювати на ASCII-аналоги (ICY-нормалізація в
// майбутньому модулі).

#include <LovyanGFX.hpp>
#include <stdint.h>

// Розміри шрифта. Номінальна висота в пікселях — display_cfg::kFont*Px.
enum class FontSize : uint8_t {
    Large,  // назва станції, заголовок меню
    Small,  // статус-рядок, тембр
    Tiny,   // підписи, дрібні позначки
};

class UiFonts {
public:
    // Створює службовий обʼєкт виміру й перевіряє, що всі розміри доступні.
    // Викликається з DisplayManager::begin(); повторний виклик безпечний.
    static bool begin();

    // Шрифт для setFont() поверх спрайту. Ніколи не nullptr.
    static const lgfx::IFont* font(FontSize size);

    // Висота рядка шрифта, пікселів (0, якщо begin() не викликано).
    static int32_t lineHeight(FontSize size);

    // Ширина рядка UTF-8 у пікселях для обраного шрифта (для marquee в ui/screens).
    // Потокобезпечна (внутрішній мʼютекс); малювання не потребує.
    static int32_t textWidth(const char* utf8, FontSize size);
};
```

**Перевірено:** твердження «підтверджено на залізі» справді має підставу — на фото реального дисплея бракувало українських літер (квадрати замість гліфів), чат виправив генератор шрифту за цим фото. `tools/gen_gfxfont.py` існує (Python + Pillow, без залежності від FreeType-CLI; покриває ASCII/Latin-1/Latin Extended-A/кирилицю U+0400–U+045F/Ґ ґ U+0490–0491, з коректно вбудованою ліцензією DejaVu Fonts у шапці згенерованого файлу). `ui/font_data.h` генерується цим скриптом; сам файл — великий масив даних, у MASTER SPEC не зберігається (лише публічний інтерфейс `ui/fonts.h` вище).

### `ui/icons.h` — реалізовано (Prompt 4)

```cpp
#pragma once

// Іконки інтерфейсу: 1-бітні маски display_cfg::kIconSize × kIconSize
// (24×24), зберігаються у flash (icons.cpp). Колір задає викликач.
//
// UiIcons лише ВІДДАЄ дані маски. Малює іконку DisplayManager::drawIcon()
// (він єдиний володіє спрайтом і викликом drawBitmap), тому UiIcons не
// потребує доступу до спрайта.
//
// Формат маски: рядки згори вниз, (kIconSize+7)/8 байт на рядок, старший
// біт зліва; 1 = піксель кольору іконки, 0 = прозорий.

#include <stddef.h>
#include <stdint.h>

enum class IconId : uint8_t {
    Wifi,      // повний сигнал (той самий бітмап, що Wifi3)
    Mute,
    Volume,
    Input,
    Standby,
    // [Prompt 4] ДОДАНО:
    WifiOff,   // немає з'єднання
    Wifi1,     // слабкий сигнал (1 дуга)
    Wifi2,     // середній сигнал (2 дуги)
    Wifi3,     // сильний сигнал (3 дуги)
};

class UiIcons {
public:
    // Кількість значень IconId (для перебору в тестовому кадрі).
    static constexpr uint8_t kCount = 9;

    // Перевіряє таблицю іконок (порядок = порядок enum, маски не nullptr).
    static bool begin();

    // Маска іконки або nullptr для невідомого id.
    static const uint8_t* mask(IconId id);

    // Короткий ASCII-підпис (для діагностики й тестового кадру, ≤ 4 символи).
    static const char* name(IconId id);

    // IconId за порядковим номером 0..kCount-1 (для перебору); поза межами — Wifi.
    static IconId fromIndex(uint8_t index);

    // Іконка Wi-Fi за кількістю «рисок» сигналу: 0 = WifiOff, 1..3 = Wifi1..Wifi3,
    // більше 3 — як 3. Відображення RSSI → рівні робить ui/screens.
    static IconId wifiForBars(uint8_t bars);
};
```

### `ui/screens.h` — заглушка (буде замінено в Prompt 6)

```cpp
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

#### `config/ir_config.h` — реалізовано (Prompt 2)

```cpp
#pragma once

// Константи модуля input/ir_rc5 (RC5/RC5X, навчання, NVS).
// Усі часові значення — у мілісекундах або мікросекундах (вказано в імені).
// Піни тут не дублюються: вони лише в config/pins.h (pins::kIrIn).

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Serial-лог кожного валідного кадру: "[IR] addr=0 cmd=16 toggle=1 rc5x=0"
// та кроків режиму навчання. Окремо від INPUT_DEBUG.
#define IR_DEBUG 1

namespace ir_cfg {

// ---------------------------------------------------------------------------
// Задача приймання (FreeRTOS)
// ---------------------------------------------------------------------------
// Ядро 0, пріоритет 4 = як у кнопок/енкодера (розділ 5 MASTER SPEC):
// вище за AppController/UI, нижче за аудіо (ядро 1). Задача майже весь час
// блокується на черзі RMT, тож не заважає.
constexpr int      kTaskCore       = 0;
constexpr uint8_t  kTaskPriority   = 4;
// Стек з запасом: у цій задачі виконується запис у NVS (Preferences) і Serial.printf.
constexpr uint32_t kTaskStackBytes = 6144;

// Період пробудження задачі без кадрів (перевірка таймаутів навчання).
constexpr uint32_t kTaskTickMs     = 50;

// Довжина черги ISR -> задача (одночасно армований лише один прийом).
constexpr uint8_t  kRxQueueLen     = 2;

// ---------------------------------------------------------------------------
// RMT RX
// ---------------------------------------------------------------------------
// Роздільність 1 МГц: 1 тік = 1 мкс (декодер трактує тривалості як мкс).
constexpr uint32_t kRmtResolutionHz    = 1000000;

// Блок памʼяті каналу RX (ESP32-S3: 48 символів на блок).
constexpr size_t   kRmtMemBlockSymbols = 48;

// Буфер прийому в символах RMT (має бути >= kRmtMemBlockSymbols).
constexpr size_t   kRmtRxBufSymbols    = 64;

// Апаратний фільтр коротких імпульсів, нс (голки). Апаратна межа ≈ 3 мкс.
constexpr uint32_t kRmtMinSignalNs     = 1000;

// Кадр вважається завершеним, якщо рівень не змінюється так довго, мкс.
// Має бути більшим за найдовший імпульс усередині кадру (2 піврівні ≈ 1778 мкс).
constexpr uint32_t kRmtIdleThresholdUs = 4000;

// Рівень піна, що означає «несуча присутня». VS1838B інвертує: LOW = 0.
constexpr uint8_t  kMarkLevel          = 0;

// Максимум сегментів (змін рівня) у кадрі, які декодер готовий розібрати.
constexpr size_t   kMaxSegments        = 32;

// ---------------------------------------------------------------------------
// Таймінги RC5
// ---------------------------------------------------------------------------
// Тривалість півбіта, мкс (RC5: 889 мкс, повний біт 1778 мкс).
constexpr uint32_t kHalfBitUs             = 889;

// Допуск на тривалість (n × півбіт), ±%. Несуча 36 кГц при 38 кГц-приймачі
// дає нижчу чутливість і «пливе» довжину імпульсів — допуск ширший за типовий.
// Має бути < 33, щоб діапазони 1 і 2 півбітів не перекривалися.
constexpr uint32_t kHalfBitTolerancePercent = 30;

// Корекція асиметрії VS1838B, мкс: додається до тривалості несучої (LOW)
// і віднімається від паузи. Підбирається за фактичними вимірами; 0 = вимкнено.
constexpr int32_t  kMarkBiasUs            = 0;

// ---------------------------------------------------------------------------
// Розрізнення натискання / утримання
// ---------------------------------------------------------------------------
// Кадри при утриманні йдуть кожні ≈114 мс. Той самий (addr, cmd, toggle)
// з паузою не більше цього порога = повтор; довша пауза = нове натискання.
constexpr uint32_t kRepeatGapMs   = 250;

// Скільки утримувати, перш ніж почати слати repeat=true
// (VOL_UP, VOL_DOWN, UP, DOWN, LEFT, RIGHT). Далі — кожен кадр (≈114 мс).
constexpr uint32_t kRepeatDelayMs = 500;

// ---------------------------------------------------------------------------
// Режим навчання
// ---------------------------------------------------------------------------
// Таймаут очікування першого й повторного натискання (кожного окремо).
constexpr uint32_t kLearnTimeoutMs         = 8000;

// Скільки чекати на confirmOverwrite() після статусу Conflict.
constexpr uint32_t kLearnConflictTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
constexpr const char* kNvsNamespace = "audioctl";
constexpr const char* kNvsMapKey    = "ir_map";   // ≤ 15 символів

// Версія бінарного формату blob (перший байт). Змінити при зміні формату
// або при перестановці значень Action.
constexpr uint8_t  kMapFormatVersion = 1;

// Максимум записів у мапі (28 дій у Action + запас).
constexpr uint8_t  kMaxMapEntries    = 40;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kRmtResolutionHz == 1000000, "decoder assumes 1 tick = 1 us");
static_assert(kRmtRxBufSymbols >= kRmtMemBlockSymbols, "RX buffer must cover one RMT memory block");
static_assert(kHalfBitTolerancePercent > 0 && kHalfBitTolerancePercent < 33,
              "tolerance must keep 1x and 2x half-bit ranges apart");
static_assert(kRmtIdleThresholdUs > (2 * kHalfBitUs * (100 + kHalfBitTolerancePercent)) / 100,
              "idle threshold must exceed the longest in-frame pulse");
static_assert(kRmtIdleThresholdUs <= 32000, "idle threshold exceeds RMT register range");
static_assert(kMaxSegments >= 28, "need room for a full RC5 frame");
static_assert(kRepeatGapMs >= 150, "repeat gap must exceed the ~114 ms RC5 frame period");
static_assert(kTaskTickMs > 0, "kTaskTickMs must be > 0");
static_assert(kMaxMapEntries >= 28 && kMaxMapEntries <= 255, "map size out of range");

}  // namespace ir_cfg
```

#### `config/audio_config.h` — реалізовано (Prompt 3)

```cpp
#pragma once

// Константи модуля audio/ (драйвери TDA7318 і PT2313L, тестовий режим).
// Піни тут не дублюються: вони лише в config/pins.h (pins::kI2cSda / kI2cScl).
// Адреса й швидкість I2C — у config/defaults.h (kAudioProcessorI2cAddr, kI2cClockHz).

#include <stddef.h>
#include <stdint.h>

#include "audio/audio_processor.h"
#include "config/defaults.h"

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Тестовий режим: Serial-команди для ручної перевірки процесора
// (audio/audio_proc_test.*). Вимкнути (0), коли зʼявиться AppController.
#define AUDIO_PROC_TEST 1

namespace audio_cfg {

// ---------------------------------------------------------------------------
// Вибір чипа на етапі тестування (поки немає NVS/Settings)
// ---------------------------------------------------------------------------
constexpr AudioProcType kTestProcType = AudioProcType::Pt2313l;

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------
// Таймаут однієї транзакції Wire (Wire.setTimeOut), мс.
constexpr uint16_t kI2cTimeoutMs = 50;

// Скільки ДОДАТКОВИХ спроб після невдалої транзакції (0 = без повторів).
// Повтор іде негайно, без паузи: пауза вимагала б delay() у драйвері.
constexpr uint8_t  kI2cRetries = 2;

// Скільки чекати на мʼютекс шини, мс (транзакція ≈ 0.3..1.2 мс при 100 кГц).
constexpr uint32_t kI2cMutexTimeoutMs = 100;

// Мінімальний інтервал між повідомленнями про помилку I2C в Serial, мс
// (щоб мертва шина не заливала лог під час покрокового ramp).
constexpr uint32_t kI2cErrLogMinIntervalMs = 1000;

// Пауза після подачі живлення перед першою командою, мс. Береться від millis()
// (час від старту), тому зазвичай у begin() очікування вже нульове.
// Джерело: примітка PT2313L про Cref = 10 мкФ (≥ 300 мс) — ЗВІРИТИ з даташитом;
// для TDA7318 вимоги немає, значення взято з запасом для обох.
constexpr uint32_t kPowerOnSettleMs = 300;

// ---------------------------------------------------------------------------
// Шкала UI (спільна для обох чипів)
// ---------------------------------------------------------------------------
constexpr int8_t kVolumeUiMin = 0;
constexpr int8_t kVolumeUiMax = 100;

// Тембр: кроки по kToneStepDb; ±7 кроків = ±14 дБ.
constexpr int8_t kToneUiMin   = -7;
constexpr int8_t kToneUiMax   = 7;
constexpr int8_t kToneStepDb  = 2;

// Баланс: 1 крок = 1 крок атенюатора = 1.25 дБ на «протилежному» каналі.
// +N = правий канал гучніший (лівий послаблюється на N кроків); -N навпаки.
constexpr int8_t kBalanceUiMin = -20;
constexpr int8_t kBalanceUiMax = 20;

// ---------------------------------------------------------------------------
// Крива гучності: UI (0..100) -> атенюація у кроках по 1.25 дБ
// ---------------------------------------------------------------------------
// Регістр гучності обох чіпів: 0..63 кроки по 1.25 дБ (0 = 0 дБ, 63 = -78.75 дБ),
// і сам байт даних дорівнює кількості кроків (0b00BBBAAA = 8*B + A).
// Крива — кусково-лінійна між якорями (значення кроків, не дБ):
//   UI 100 ->  0 кроків (  0.0 дБ)     UI 50 -> 16 кроків (-20 дБ)
//   UI  75 ->  8 кроків (-10.0 дБ)     UI 25 -> 32 кроки  (-40 дБ)
//   UI   0 -> 63 кроки  (-78.75 дБ)
// Значення підбирається на слух; змінювати лише таблицю якорів.
constexpr uint8_t kVolMaxAttSteps = 63;
constexpr uint8_t kVolCurveLen    = 5;
constexpr uint8_t kVolCurveUi[kVolCurveLen]    = {0, 25, 50, 75, 100};
constexpr uint8_t kVolCurveSteps[kVolCurveLen] = {63, 32, 16, 8, 0};

// Кроки атенюації (0..63) для UI-гучності. Значення поза межами обрізаються.
constexpr uint8_t volumeAttSteps(int ui) {
    if (ui <= kVolCurveUi[0]) {
        return kVolCurveSteps[0];
    }
    if (ui >= kVolCurveUi[kVolCurveLen - 1]) {
        return kVolCurveSteps[kVolCurveLen - 1];
    }
    uint8_t i = 1;
    while (ui > kVolCurveUi[i]) {
        ++i;
    }
    const int u0 = kVolCurveUi[i - 1];
    const int u1 = kVolCurveUi[i];
    const int s0 = kVolCurveSteps[i - 1];
    const int s1 = kVolCurveSteps[i];
    const int num = (s1 - s0) * (ui - u0);   // ≤ 0: атенюація спадає зі зростанням UI
    const int den = u1 - u0;
    // Округлення до найближчого цілого без float.
    const int delta = (num >= 0) ? (2 * num + den) / (2 * den)
                                 : -((-2 * num + den) / (2 * den));
    return static_cast<uint8_t>(s0 + delta);
}

constexpr bool volCurveValid() {
    if (kVolCurveUi[0] != kVolumeUiMin || kVolCurveUi[kVolCurveLen - 1] != kVolumeUiMax) {
        return false;
    }
    for (uint8_t i = 1; i < kVolCurveLen; ++i) {
        if (kVolCurveUi[i] <= kVolCurveUi[i - 1]) return false;       // UI зростає
        if (kVolCurveSteps[i] > kVolCurveSteps[i - 1]) return false;  // атенюація не зростає
    }
    return kVolCurveSteps[0] <= kVolMaxAttSteps;
}

// ---------------------------------------------------------------------------
// Входи
// ---------------------------------------------------------------------------
// Фізичні стерео-входи чипів (за даташитами):
//   TDA7318 — 4 (Stereo 1..4);
//   PT2313L — 3 (Stereo 1..3; код Stereo 4 у чипі не виведений назовні).
// Логічний індекс 0..N-1 = Stereo (N+1). У проєкті 4 логічні входи (defaults::kInputCount),
// тож з PT2313L доступні лише 0..2; вхід 3 (Aux) — див. «Відомі обмеження».
constexpr uint8_t kTda7318InputCount = 4;
constexpr uint8_t kPt2313lInputCount = 3;

// Підсилення входу в КРОКАХ над 0 дБ (0..3), по одному значенню на логічний вхід.
//   TDA7318: крок 6.25 дБ (0, +6.25, +12.5, +18.75)
//   PT2313L: крок 3.75 дБ (0, +3.75, +7.5,  +11.25)
constexpr uint8_t kInputGainSteps[defaults::kInputCount] = {0, 0, 0, 0};

// ---------------------------------------------------------------------------
// Варіант корпусу PT2313L
// ---------------------------------------------------------------------------
// 28-pin (DIP/SO) має тембр і loudness; 20-pin (SSOP) — не має.
// Автовизначення неможливе. Для 20-pin поставити false.
constexpr bool kPt2313lHasToneLoudness = true;

// ---------------------------------------------------------------------------
// Тестовий режим (AUDIO_PROC_TEST)
// ---------------------------------------------------------------------------
// Ядро 0, пріоритет 1 (нижче введення = 4, AppController ≈ 3, UI ≈ 2).
constexpr int      kTestTaskCore       = 0;
constexpr uint8_t  kTestTaskPriority   = 1;
constexpr uint32_t kTestTaskStackBytes = 4096;

// Період опитування Serial, мс.
constexpr uint32_t kTestPollMs         = 20;

// Максимальна довжина рядка команди разом із '\0'.
constexpr size_t   kTestLineMax        = 24;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
static_assert(kVolumeUiMin == 0 && kVolumeUiMax == 100, "UI volume scale is 0..100");
static_assert(kVolMaxAttSteps == 63, "volume register is 6 bits: 0..63");
static_assert(volCurveValid(), "volume curve anchors must be monotonic and span 0..100");
static_assert(volumeAttSteps(kVolumeUiMax) == 0, "UI max must be 0 dB");
static_assert(volumeAttSteps(kVolumeUiMin) == kVolMaxAttSteps, "UI min must be max attenuation");
static_assert(volumeAttSteps(defaults::kDefaultVolume) == 22, "default volume 40 -> 22 steps (-27.5 dB)");
static_assert(kToneUiMin == -kToneUiMax && kToneUiMax <= 7, "tone: symmetric, 4-bit code allows +-7 steps");
static_assert(kBalanceUiMin == -kBalanceUiMax, "balance range must be symmetric");
static_assert(kBalanceUiMax <= 30, "speaker attenuator: 30 steps max before mute code");
static_assert(kTda7318InputCount <= defaults::kInputCount, "more chip inputs than logical inputs");
static_assert(kPt2313lInputCount <= defaults::kInputCount, "more chip inputs than logical inputs");
static_assert(kInputGainSteps[0] <= 3 && kInputGainSteps[1] <= 3 &&
              kInputGainSteps[2] <= 3 && kInputGainSteps[3] <= 3,
              "input gain is a 2-bit field: 0..3 steps");
static_assert(kI2cTimeoutMs > 0, "kI2cTimeoutMs must be > 0");
static_assert(kTestLineMax >= 8, "kTestLineMax too small");

}  // namespace audio_cfg
```

#### `config/display_config.h` — реалізовано (Prompt 4)

```cpp
#pragma once

// Константи модуля ui/ (display, fonts, icons).
// Піни тут не дублюються: вони лише в config/pins.h (pins::kSt7789*).
// Яскравість за замовчуванням — defaults::kDefaultBrightness (config/defaults.h).

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Прапорці умовної компіляції
// ---------------------------------------------------------------------------

// Тестовий кадр (статичний, без анімації, малюється один раз при старті):
// рамка + кольорові кути (перевірка зміщення вікна), рядки з українськими
// літерами трьома розмірами, алфавіт, усі IconId з підписами.
// Не заміна ui/screens. Вимкнути (0), коли зʼявляться екрани.
#define DISPLAY_DEMO 1

namespace display_cfg {

// ---------------------------------------------------------------------------
// Геометрія
// ---------------------------------------------------------------------------
// Робоча (горизонтальна) роздільність: 320×170.
constexpr int16_t kWidth  = 320;
constexpr int16_t kHeight = 170;

// Фізична (портретна) матриця панелі: 170 (коротка сторона) × 320.
constexpr int16_t kPanelWidth  = 170;
constexpr int16_t kPanelHeight = 320;

// Памʼять кадру самого ST7789 — 240×320 (панель 170 пікселів показує лише
// частину цього вікна, звідси зміщення).
constexpr int16_t kMemoryWidth  = 240;
constexpr int16_t kMemoryHeight = 320;

// ПРИПУЩЕННЯ, ПЕРЕВІРИТИ НА ЗАЛІЗІ (див. «Перевірити на залізі»):
// зміщення вікна в ПОРТРЕТНІЙ системі координат панелі (rotation 0).
// (240 − 170) / 2 = 35 — типове значення для 170×320 ST7789 (напр. LilyGO
// T-Display-S3). LovyanGFX сам перераховує зміщення при повороті, тому тут
// вказується саме портретне. Якщо кути/рамка зʼїхали — змінювати ці два числа
// (варіанти: 0/35, 35/0, 0/0, 70/0).
constexpr int16_t kOffsetX = 35;
constexpr int16_t kOffsetY = 0;

// Поворот LovyanGFX: 1 = ландшафт 320×170; 3 = той самий ландшафт, розвернутий
// на 180°. Дисплей у корпусі може стояти в будь-який бік, тому орієнтація
// перемикається під час роботи: DisplayManager::setFlipped(bool) — це буде
// налаштування в Settings/вебі. Розміри 320×170 в обох випадках однакові.
constexpr uint8_t kRotationNormal  = 1;
constexpr uint8_t kRotationFlipped = 3;

// Орієнтація при старті, поки Settings немає. true = kRotationFlipped.
// Перевірено на макетці: з виводами модуля праворуч правильний напрямок
// тексту дає саме kRotationFlipped (при kRotationNormal картинка догори дригом).
constexpr bool kDefaultFlipped = true;

// ПРИПУЩЕННЯ, ПЕРЕВІРИТИ: більшість модулів 170×320 потребують інверсії кольорів.
// Ознака помилки: чорний фон виглядає білим. Тоді змінити на false.
constexpr bool kInvertColors = true;

// ПРИПУЩЕННЯ, ПЕРЕВІРИТИ: порядок RGB/BGR. Ознака помилки: червоний кут
// (лівий верхній) виглядає синім. Тоді змінити на true.
constexpr bool kBgrOrder = false;

// ---------------------------------------------------------------------------
// SPI
// ---------------------------------------------------------------------------
// Піни SCLK12/MOSI11/CS10 — нативні IOMUX-лінії FSPI, тож шина витримує до
// 80 МГц. Початкове безпечне значення — 40 МГц. Підняти до 80 МГц можна
// ПІСЛЯ перевірки на залізі (артефакти: «сніг», зсунуті рядки, збої кольору
// на довгих провідниках/макетці). Час передачі кадру = 320·170·16 біт / f:
// 40 МГц ≈ 22 мс, 80 МГц ≈ 11 мс.
constexpr uint32_t kSpiWriteHz = 80000000;
constexpr uint8_t  kSpiMode    = 0;

// Передача кадру через DMA (кадр у PSRAM). За замовчуванням ВИМКНЕНО: поведінка
// DMA з PSRAM залежить від версії LovyanGFX/IDF і не перевірена на залізі.
// Увімкнути (true) після успішної перевірки: тоді передача кадру A йде у фоні,
// поки задача малює кадр B у другий буфер.
constexpr bool kUseDma = false;

// ---------------------------------------------------------------------------
// Підсвітка (LEDC PWM, лише ledcAttach())
// ---------------------------------------------------------------------------
// 20 кГц — вище за чутний діапазон (5 кГц давали б писк у аналоговому тракті
// поруч з аудіопроцесором). 10 біт: 80 МГц / (20 кГц · 1024) ≈ 3.9 — дільник
// LEDC у допустимих межах.
constexpr uint32_t kBacklightFreqHz         = 20000;
constexpr uint8_t  kBacklightResolutionBits = 10;
// true: високий рівень на BLK = підсвітка ввімкнена.
constexpr bool     kBacklightActiveHigh     = true;

// ---------------------------------------------------------------------------
// Задача малювання (FreeRTOS)
// ---------------------------------------------------------------------------
// Ядро 0 (UI), пріоритет 2 — за таблицею розділу 5 MASTER SPEC:
// введення = 4, AppController ≈ 3, UI = 2, мережа/веб ≈ 1. Число зі
// специфікації не змінювалось. Аудіотест (audio_cfg::kTestTaskPriority) = 1.
constexpr int      kTaskCore       = 0;
constexpr uint8_t  kTaskPriority   = 2;
constexpr uint32_t kTaskStackBytes = 8192;

// Період кадру, мс. 40 мс = 25 кадр/с: для VU-метра й UI достатньо плавно;
// при 40 МГц передача кадру ≈ 22 мс (без DMA) + малювання ≈ 5..10 мс
// вкладаються в 40 мс, а 33 мс (30 кадр/с) лишили б замало запасу.
constexpr uint32_t kFramePeriodMs = 40;

// Кількість повнокадрових буферів (спрайтів) у PSRAM: 2 × 320·170·2 ≈ 217 КБ.
constexpr uint8_t  kBufferCount = 2;

// ---------------------------------------------------------------------------
// Шрифти: власні GFXfont з DejaVu Sans (ui/font_data.h, генерує
// tools/gen_gfxfont.py). Значення — висота рядка (yAdvance) у пікселях; вони
// мусять збігатися з тими, що надрукує UiFonts::begin() у Serial.
// ---------------------------------------------------------------------------
constexpr uint8_t kFontLargePx = 24;   // FontSize::Large — назва станції, заголовок меню
constexpr uint8_t kFontSmallPx = 17;   // FontSize::Small — статус-рядок, тембр
constexpr uint8_t kFontTinyPx  = 14;   // FontSize::Tiny  — підписи, дрібні позначки

// ---------------------------------------------------------------------------
// Іконки: 1-бітна маска kIconSize × kIconSize, рядки по (kIconSize+7)/8 байт,
// старший біт зліва; колір задає викликач.
// ---------------------------------------------------------------------------
constexpr uint8_t kIconSize = 24;

// ---------------------------------------------------------------------------
// Кольори (RGB565, тип uint16_t — LovyanGFX трактує його як RGB565)
// ---------------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t kColorBlack  = rgb565(0, 0, 0);
constexpr uint16_t kColorWhite  = rgb565(255, 255, 255);
constexpr uint16_t kColorRed    = rgb565(255, 0, 0);
constexpr uint16_t kColorGreen  = rgb565(0, 255, 0);
constexpr uint16_t kColorBlue   = rgb565(0, 0, 255);
constexpr uint16_t kColorYellow = rgb565(255, 220, 0);
constexpr uint16_t kColorGray   = rgb565(140, 140, 140);

// Кольори за замовчуванням для примітивів.
constexpr uint16_t kColorBg = kColorBlack;
constexpr uint16_t kColorFg = kColorWhite;

// ---------------------------------------------------------------------------
// Розкладка тестового кадру (DISPLAY_DEMO). Лише для перевірки заліза.
// ---------------------------------------------------------------------------
namespace demo {
constexpr int16_t kFrameThickness = 2;    // товщина рамки по краю
constexpr int16_t kCornerSize     = 10;   // кольорові квадрати в кутах
constexpr int16_t kCornerInset    = 2;    // відступ квадратів від краю (всередину рамки)
constexpr int16_t kTextX          = 16;   // лівий відступ тексту й ряду іконок
constexpr int16_t kTitleY         = 14;   // Large: «Ґрунт Їжачок Єдність»
constexpr int16_t kSubtitleY      = 44;   // Small
constexpr int16_t kAlphaUpperY    = 68;   // Tiny: великі літери
constexpr int16_t kAlphaLowerY    = 84;   // Tiny: малі літери
constexpr int16_t kIconRowY       = 102;  // верх ряду іконок
constexpr int16_t kIconSlotW      = 32;   // ширина комірки іконки з підписом (9 × 32 = 288)
constexpr int16_t kCaptionY       = 130;  // Tiny: підписи під іконками
constexpr int16_t kInfoY          = 148;  // Tiny: параметри панелі
}  // namespace demo

}  // namespace display_cfg
```

_(наступний модуль додасть сюди константи аудіоплеєра/радіо після Prompt 5)_