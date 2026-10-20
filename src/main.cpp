// Точка входу скелета проекту.
// Завдання цього файла:
// - ініціалізувати Serial;
// - надрукувати інформацію про чіп, PSRAM, flash і розділи;
// - ініціалізувати LittleFS (з форматуванням, якщо потрібно);
// - підготувати подієву шину;
// - запустити кнопки, енкодер і IR та (тимчасово) друкувати їхні події;
// - [Prompt 48a] аудіочіпи (TDA7318 / PT2313L), I2C і AudioProcTest ВИРІЗАНО: замість ініціалізації
//   процесора — input_selector::begin() (реле входів TV Box / Computer, початковий стан = TV Box);
// - [Prompt 4] ініціалізувати дисплей (ST7789, спрайт у PSRAM, задача малювання;
//   під DISPLAY_DEMO — статичний тестовий кадр);
// - [Prompt 5] створити аудіоплеєр інтернет-радіо (ESP32-audioI2S) і, під
//   AUDIO_PLAYER_TEST, запустити його Serial-тестовий режим;
// - [Prompt 6] ініціалізувати Settings (NVS) ДО дисплея:
//   орієнтація дисплея береться з Settings; під SETTINGS_TEST —
//   Serial-тест Settings (команди з префіксом `set.`).
// - [Prompt 8] запустити AppController (читає EventBus, керує звуком/плеєром/станом);
//   тимчасовий друк подій (INPUT_DEMO_PRINT_EVENTS) прибрано, loop() порожній.
// - [Prompt 11] StationStore стартує всередині AppController::begin(); тут лише
//   Serial-тест станцій (команди `st.*`).
// - [Prompt 12] WifiManager::begin(wifiResetRequested) викликається тут, окремо від
//   AppController::begin() (сигнатуру AppController не змінено); WifiManager сам
//   піднімає mDNS після підключення.
// - [Prompt 13] WebServerManager::begin() викликається ПІСЛЯ AppController::begin()
//   (потрібні AppState і Settings); сервер сам піднімається, коли WifiManager дасть STA.
// - [Prompt 47d] adc_vu видалено: VU зовнішніх входів виробляє line_path (initAdcVu() більше немає).
// - [Prompt 23c] Перший рядок setup() — AmpStandby::begin(): пін standby підсилювача в LOW
//   до ініціалізації аудіо; HIGH ставить лише AppController (powerOnTransition()).

#include <Arduino.h>
#include <LittleFS.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "audio/line_path.h"          // [Prompt 44] ДОДАНО
#include "audio/amp_standby.h"        // [Prompt 23c] ДОДАНО
#include "audio/audio_player.h"       // [Prompt 5] ДОДАНО
#include "audio/audio_player_test.h"  // [Prompt 5] ДОДАНО
#include "audio/input_selector.h"     // [Prompt 48a] реле-селектор входів
#include "config/audio_player_config.h"  // [Prompt 5] ДОДАНО
#include "config/defaults.h"
#include "config/display_config.h"  // [Prompt 6] ДОДАНО (display_cfg::kDefaultFlipped)
#include "config/features.h"
#include "config/input_config.h"
#include "config/line_path_config.h"  // [Prompt 44] ДОДАНО
#include "config/pins.h"
#include "config/settings_config.h"  // [Prompt 6] ДОДАНО
#include "config/station_store_config.h"  // [Prompt 11] ДОДАНО
#include "core/app_controller.h"  // [Prompt 8] ДОДАНО
#include "core/events.h"
#include "core/settings.h"       // [Prompt 6] ДОДАНО
#include "core/settings_test.h"  // [Prompt 6] ДОДАНО
#include "input/buttons.h"
#include "input/encoder.h"
#include "input/ir_rc5.h"  // [Prompt 2] ДОДАНО
#include "net/web_server.h"   // [Prompt 13] ДОДАНО
#include "net/wifi_manager.h"  // [Prompt 12] ДОДАНО
#include "stations/station_store_test.h"  // [Prompt 11] ДОДАНО
#include "ui/display.h"    // [Prompt 4] ДОДАНО
#include "ui/screens.h"   // [Prompt 9] ДОДАНО

// Друк інформації про чіп і памʼять.
static void printChipInfo() {
    Serial.printf("[MAIN] Chip: %s rev%d, cores=%d\n",
                  ESP.getChipModel(),
                  ESP.getChipRevision(),
                  ESP.getChipCores());

    Serial.printf("[MAIN] Flash: %u MB\n",
                  static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)));

    Serial.printf("[MAIN] PSRAM: %u bytes\n",
                  static_cast<unsigned>(ESP.getPsramSize()));

    if (!psramFound()) {
        Serial.println("[MAIN] PSRAM not found");
    }

    Serial.printf("[MAIN] Free heap: %u bytes\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));

#if ENABLE_BT_UART
    Serial.println("[MAIN] Feature BT-UART enabled");
#else
    Serial.println("[MAIN] Feature BT-UART disabled");
#endif

#if ENABLE_VU
    Serial.println("[MAIN] Feature VU enabled");
#endif
}

// Друк таблиці розділів і поточної активної партіції.
static void printPartitionInfo() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        Serial.printf("[MAIN] Running partition: %s\n", running->label);
    }

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);

    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        if (part != nullptr) {
            Serial.printf("[MAIN]   partition: %-10s offset=0x%08lx size=0x%08lx\n",
                          part->label,
                          static_cast<unsigned long>(part->address),
                          static_cast<unsigned long>(part->size));
        }
        it = esp_partition_next(it);
    }

    esp_partition_iterator_release(it);
}

// Ініціалізація LittleFS з автоформатуванням.
// Якщо монтування не вдалося (наприклад, розділ порожній або пошкоджений),
// намагаємося відформатувати і змонтувати знову.
static void initLittleFs() {
    // Спроба 1: монтування без форматування.
    if (LittleFS.begin(false, defaults::kLittleFsMountPoint)) {
        Serial.printf("[MAIN] LittleFS mounted at %s\n", defaults::kLittleFsMountPoint);
        Serial.printf("[MAIN] LittleFS total: %u bytes\n",
                      static_cast<unsigned>(LittleFS.totalBytes()));
        Serial.printf("[MAIN] LittleFS used: %u bytes\n",
                      static_cast<unsigned>(LittleFS.usedBytes()));
        return;
    }

    // Спроба 2: форматування і монтування.
    Serial.println("[MAIN] LittleFS mount failed, formatting...");
    if (!LittleFS.begin(true, defaults::kLittleFsMountPoint)) {
        Serial.println("[MAIN] LittleFS format and mount failed");
        return;
    }

    Serial.printf("[MAIN] LittleFS formatted and mounted at %s\n", defaults::kLittleFsMountPoint);
    Serial.printf("[MAIN] LittleFS total: %u bytes\n",
                  static_cast<unsigned>(LittleFS.totalBytes()));
    Serial.printf("[MAIN] LittleFS used: %u bytes\n",
                  static_cast<unsigned>(LittleFS.usedBytes()));
}

// [Prompt 48a] Реле-селектор входів: пін як вихід, реле вимкнено (TV Box). Рання ініціалізація (до плеєра й
// AppController), щоб стан реле був визначений до першого звуку. Радіо реле не чіпає.
static void initInputSelector() {
    if (!input_selector::begin()) {
        Serial.println("[MAIN] Input selector init failed");
    }
}

// [Prompt 5] ДОДАНО: аудіоплеєр. [Prompt 48a] Аудіопроцесора більше немає: у AudioPlayer::begin()
// більше не передається (begin() без параметрів).
static void initAudioPlayer() {
    if (!AudioPlayer::begin()) {
        Serial.println("[MAIN] Audio player init failed");
        return;
    }
    Serial.println("[MAIN] Audio player ready");

#if AUDIO_PLAYER_TEST
    if (AudioPlayerTest::begin()) {
        Serial.println("[MAIN] Player test mode ready (send 'help')");
    } else {
        Serial.println("[MAIN] Player test mode init failed");
    }
#endif
}

// [Prompt 4] ДОДАНО: дисплей. DisplayManager::begin() сам ініціалізує шрифти
// й іконки, панель, буфери в PSRAM і запускає задачу малювання.
static void initDisplay() {
    if (DisplayManager::begin()) {
        Serial.println("[MAIN] Display ready");
        // [Prompt 6] ДОДАНО: begin() стартує з display_cfg::kDefaultFlipped; якщо
        // в Settings інша орієнтація — перемикаємо. Рівне значення пропускаємо,
        // щоб не робити зайвий поворот панелі.
        UiScreens::begin();   // [Prompt 9] ДОДАНО: після begin() дисплея
        const bool flipped = SettingsStore::get().displayFlipped;
        if (flipped != display_cfg::kDefaultFlipped) {
            DisplayManager::setFlipped(flipped);
            Serial.printf("[MAIN] Display orientation from Settings: %s\n",
                          flipped ? "flipped" : "normal");
        }
        return;
    }

    switch (DisplayManager::lastError()) {
        case DisplayError::NoPsram:
            Serial.println("[MAIN] Display disabled: PSRAM not found");
            break;
        case DisplayError::PanelInitFailed:
            Serial.println("[MAIN] Display panel init failed");
            break;
        default:
            Serial.printf("[MAIN] Display init failed: %s\n", DisplayManager::lastErrorName());
            break;
    }
}

// [Prompt 6] ДОДАНО: Settings (NVS). Має відпрацювати ДО initDisplay():
// він читає SettingsStore::get(). Збій NVS не зупиняє старт —
// у кеші лежать значення за замовчуванням.
static void initSettings() {
    if (!SettingsStore::begin()) {
        Serial.println("[MAIN] Settings init failed (NVS/task), RAM defaults in use");
    }
    if (!SettingsStore::load()) {
        Serial.println("[MAIN] Settings load/save failed, RAM defaults in use");
    }

    const Settings& s = SettingsStore::get();
    Serial.printf("[MAIN] Settings: flipped=%d brightness=%u\n", s.displayFlipped ? 1 : 0,
                  static_cast<unsigned>(s.brightness));
}

// [Prompt 6] ДОДАНО: Serial-тест Settings. Читає той самий Serial, що й
// AudioPlayerTest, тому за замовчуванням не стартує, поки
// він активний (див. SETTINGS_TEST_SHARE_SERIAL у settings_config.h).
static void initSettingsTest() {
#if SETTINGS_TEST
#if AUDIO_PLAYER_TEST && !SETTINGS_TEST_SHARE_SERIAL
    Serial.println("[MAIN] Settings test skipped: Serial is used by another test "
                   "(set AUDIO_PLAYER_TEST to 0 or SETTINGS_TEST_SHARE_SERIAL to 1)");
#else
    if (SettingsTest::begin()) {
        Serial.println("[MAIN] Settings test mode ready (send 'set.help')");
    } else {
        Serial.println("[MAIN] Settings test mode init failed");
    }
#endif
#endif
}

// [Prompt 12] ДОДАНО: Wi-Fi менеджер. Створює власну задачу й одразу повертається;
// forceReset = утримання OK при старті (стерти збережену мережу -> AP).
static void initWifi(bool forceReset) {
    if (WifiManager::begin(forceReset)) {
        Serial.println("[MAIN] WifiManager started");
    } else {
        Serial.println("[MAIN] WifiManager init failed");
    }
}

// [Prompt 8] ДОДАНО: AppController. Останній з модулів, від яких він залежить
// (EventBus, Settings, input_selector, плеєр, line_path). [P48a] begin() без параметрів.
static void initAppController() {
    if (AppController::begin()) {
        Serial.println("[MAIN] AppController ready");
    } else {
        Serial.println("[MAIN] AppController init failed");
    }
}

// [Prompt 44] ДОДАНО: прототип проходу PCM1808 -> PCM5102 (I2S1 duplex slave). Після AudioPlayer::begin()
// (там I2S0 отримує піни й MCLK, а XSMT переходить у мʼют). Запуск лише з Serial-тесту (lp.*).
// [Prompt 44b] ЗМІНЕНО: begin() більше не створює канали I2S1, задачу й буфери (їх створює start() і
// звільняє returnToRadio()); при LINE_PATH_ENABLE == 0 і LINE_PATH_TEST == 1 стартує лише Serial-тест
// з командою lp.heap (еталонний замір RAM без line_path).
static void initLinePath() {
#if LINE_PATH_TEST
#if AUDIO_PLAYER_TEST
    Serial.println("[MAIN] WARNING: line path test shares Serial with another test (lp.* lines are ignored by others)");
#endif
#if STATION_STORE_TEST && !STATION_STORE_TEST_SHARE_SERIAL
    // Два читачі Serial забирають байти один в одного: частина lp.* команд «зникає».
    Serial.println("[MAIN] WARNING: STATION_STORE_TEST reads Serial too - set it to 0 in "
                   "station_store_config.h, otherwise some lp.* commands are lost");
#endif
#endif
#if LINE_PATH_ENABLE
    if (LinePath::begin()) {
        Serial.println("[MAIN] Line path ready (I2S1 resources are created on start())");
    } else {
        Serial.println("[MAIN] Line path init failed");
    }
#elif LINE_PATH_TEST
    LinePath::begin();  // лише Serial-тест (lp.heap); сам модуль вимкнено
    Serial.println("[MAIN] Line path disabled (LINE_PATH_ENABLE = 0): test mode has lp.heap only");
#endif
}

// [Prompt 13] ДОДАНО: веб-сервер. [P48a] begin() без параметрів.
// begin() лише створює задачу-стартер; HTTP піднімається, коли WifiManager дасть STA.
static void initWebServer() {
    if (WebServerManager::begin()) {
        Serial.println("[MAIN] WebServer starter task created");
    } else {
        Serial.println("[MAIN] WebServer init failed");
    }
}

// [Prompt 11] ДОДАНО: Serial-тест StationStore (після AppController, який
// викликає StationStore::begin()). Кілька читачів Serial діляться байтами, тому
// не стартуємо, поки активний інший тест (на відміну від них, тут перевіряємо й
// SETTINGS_TEST): знімається лише STATION_STORE_TEST_SHARE_SERIAL = 1.
static void initStationStoreTest() {
#if STATION_STORE_TEST
#if (AUDIO_PLAYER_TEST || SETTINGS_TEST) && !STATION_STORE_TEST_SHARE_SERIAL
    Serial.println("[MAIN] Station store test skipped: Serial is used by another test "
                   "(set AUDIO_PLAYER_TEST/SETTINGS_TEST to 0 or "
                   "STATION_STORE_TEST_SHARE_SERIAL to 1)");
#else
    if (StationStoreTest::begin()) {
        Serial.println("[MAIN] Station store test mode ready (send 'st.help')");
    } else {
        Serial.println("[MAIN] Station store test mode init failed");
    }
#endif
#endif
}

void setup() {
#if FEATURE_AMP_STANDBY
    // [Prompt 23c] Найраніше, що можна зробити в прошивці: пін standby підсилювача в LOW.
    const bool ampPinOk = AmpStandby::begin();
#endif

    Serial.begin(defaults::kSerialBaud);

    // Коротка пауза для стабілізації логу.
    vTaskDelay(pdMS_TO_TICKS(50));

    Serial.println("[MAIN] ESP32-S3 Audio Controller skeleton");
    Serial.println("[MAIN] Build: " __DATE__ " " __TIME__);
#if FEATURE_AMP_STANDBY
    Serial.printf("[MAIN] Amp standby pin GPIO%d: %s\n", pins::kAmpStby,
                  ampPinOk ? "standby level set" : "init failed");
#else
    Serial.println("[MAIN] Amp standby control disabled");
#endif

    // Утримання OK при старті = запит скидання Wi-Fi.
    // Перевірка йде ДО запуску задач і не залежить від EventBus.
    // Без утримання повертається за ~5 мс.
    // [Prompt 12] wifiResetRequested передається в WifiManager::begin() нижче.
    const bool wifiResetRequested =
        Buttons::isHeldAtBoot(pins::kBtnOk, input_cfg::kBootWifiResetHoldMs);
    if (wifiResetRequested) {
        Serial.println("[MAIN] OK held at boot: Wi-Fi reset requested");
    }

    printChipInfo();
    printPartitionInfo();
    initLittleFs();

    // [Prompt 6] ДОДАНО: Settings до аудіопроцесора й дисплея.
    initSettings();

    if (!EventBus::begin()) {
        Serial.println("[MAIN] EventBus init failed");
    } else {
        Serial.println("[MAIN] EventBus ready");
    }

    if (Buttons::begin()) {
        Serial.println("[MAIN] Buttons ready");
    } else {
        Serial.println("[MAIN] Buttons init failed");
    }

    if (Encoder::begin()) {
        Serial.println("[MAIN] Encoder ready");
    } else {
        Serial.println("[MAIN] Encoder init failed");
    }

    // [Prompt 2] ДОДАНО: IR-приймач після енкодера.
    if (IrRc5::begin()) {
        Serial.println("[MAIN] IR ready");
    } else {
        Serial.println("[MAIN] IR init failed");
    }

    // [Prompt 48a] Реле-селектор входів (замість колишнього аудіопроцесора): TV Box до першого вибору входу.
    initInputSelector();

    // [Prompt 4] ДОДАНО: дисплей.
    initDisplay();

    // [Prompt 5] ДОДАНО: аудіоплеєр.
    initAudioPlayer();

    // [Prompt 12] ДОДАНО: Wi-Fi після плеєра, але до AppController, щоб перший
    // синхронізований стан AppController уже бачив Connecting/AP.
    initWifi(wifiResetRequested);

    // [Prompt 44] ДОДАНО. [Prompt 47c] ПЕРЕНЕСЕНО ПЕРЕД AppController: begin() проходу має відбутись до першого
    // powerOnTransition() контролера (стартує на зовнішньому вході й одразу керує LinePath).
    initLinePath();

    // [Prompt 8] ДОДАНО: AppController після input_selector, плеєра, line_path й Settings.
    initAppController();

    // [Prompt 47d] initAdcVu() видалено: VU зовнішніх входів тепер виробляє line_path (ініціалізується вище).

    // [Prompt 13] ДОДАНО: веб-сервер після AppController (AppState::begin() уже виконано).
    initWebServer();

    // [Prompt 6] ДОДАНО: Serial-тест Settings (останнім, щоб не заважати логу старту).
    initSettingsTest();

    // [Prompt 11] ДОДАНО
    initStationStoreTest();

    Serial.println("[MAIN] Skeleton ready");
}

void loop() {
    // [Prompt 8] ЗМІНЕНО: уся логіка — в задачах (AppController, введення, аудіо,
    // дисплей). loopTask живе на ядрі 1 поруч з аудіо, тому лише спить.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
