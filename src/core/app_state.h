#pragma once

// Єдине сховище стану пристрою.
// Доступ потокобезпечний (мʼютекс всередині).
// [Prompt 6] Реалізовано core/app_state.cpp; ДОДАНО modify() (два варіанти).
// [Prompt 8] begin() викликає AppController (першим ділом у своєму begin()).
// [Prompt 8] ДОДАНО: enum AdjustTarget, enum MenuContext та чотири поля В КІНЕЦЬ
// AppStateData (gain, adjustTarget, menuContext, menuSelection). Нові поля
// додано лише в кінець структури, тому код, що ініціалізує наявні поля за
// іменами чи агрегатно, лишається чинним (нові поля нульові).
// [Prompt 10] ДОДАНО: enum StreamStatus та поле streamStatus для детального
// статусу потоку у UI. Незалежно від audio/audio_player.h.
// [Prompt 12] ДОДАНО: wifiSsid, wifiIp, wifiApMode (лише в кінець структури) —
// дані net/wifi_manager для екрана Mode::WifiSetup. wifiConnected (Prompt 6)
// тепер заповнюється з WifiManager::isConnected().
// [Prompt 15] ДОДАНО: enum IrLearnStatus та поля irLearnTarget, irLearnStatus,
// irLearnConflictWith (лише в кінець структури) — стан навчання IR-пульта для
// екрана Mode::IrLearn та веб-API. Нульові значення = Idle. Підключено
// core/events.h (потрібен лише тип Action).
// [Prompt 16] ДОДАНО: Mode::OtaUpdate (в кінець enum) та поле otaProgress (в кінець
// структури) — прогрес запису прошивки для екрана та веб-API. Окремого статусу
// помилки немає: при невдачі AppController одразу повертає попередній Mode.
// [Prompt 21b] ДОДАНО: loudness (лише в кінець структури). volume/bass/treble/balance/gain/
// loudness тепер дзеркалять ПРОФІЛЬ ПОТОЧНОГО входу (AppController міняє їх при перемиканні входу).
// [Prompt 23b] ДОДАНО: inputName (лише в кінець структури) — користувацька назва ПОТОЧНОГО входу
// з Settings::inputNames (запасно defaults::kInputNames). Вирішує AppController (resolveInputName),
// ui/screens лише читає поле.
// [Prompt 28] ДОДАНО: offline та restarting (лише в кінець структури). offline — режим без мережі
// (Wi-Fi вимкнено, радіо недоступне); restarting — іде «тихий» перезапуск контролера (екран
// показує «Restarting...»). Обидва false за замовчуванням; публікує AppController.
// [Prompt 32] ДОДАНО: popupTarget, popupSeq (лише в кінець структури) — подія «показати спливне вікно
// параметра звуку». popupSeq збільшує (з переходом 255 -> 0) ЛИШЕ AppController на ЛОКАЛЬНІ зміни
// (енкодер, кнопки, IR); веб-команди його не чіпають. Відлік показу (kPopupTimeoutMs) веде ui/screens.
// [Prompt 36] ДОДАНО: wifiRssi (лише в кінець структури) — рівень сигналу поточної STA-мережі, дБм,
// квантований кроком wifi_status_cfg::kRssiQuantDb; 0 = невідомо / не підключено. Публікує
// AppController разом з іншими даними Wi-Fi (syncPlayer); ui/screens з нього виводить іконку Wi-Fi.
// [Prompt 39] ДОДАНО: inputEnabledMask (лише в кінець структури) — маска входів, дозволених
// користувачем (біт i = вхід i, біт 0 завжди 1). Публікує AppController (старт, зміна з вебу);
// апаратну доступність входів (inputCount чипа) вона НЕ враховує. До першого publishState() у
// AppController::begin() значення 0 (нульова ініціалізація) не має сенсу — не читати раніше.
// [Prompt 49a] ДОДАНО: MenuContext::EqBalance (екран «EQ + баланс» = Mode::Menu + цей контекст, за зразком
// StationList) та поля eqSelected, eqGainsDb[5] (лише в кінець структури). Публікує AppController, поки екран
// відкритий; поза ним нулі.
// [Prompt 50b] ВИДАЛЕНО поля bass, treble, balance (legacy-нуль; баланс активного входу — balanceDb), gain, loudness;
// AdjustTarget тепер {Volume, Balance}.
// [V1] ДОДАНО: vuSensDb (лише в кінець структури) — чутливість VU АКТИВНОГО входу, дБ (vu_sens_cfg::kMinDb..kMaxDb,
// 0 = нейтрально). Публікує AppController (publishState). Нуль = дефолт, тож app_state.cpp не змінюється.
// [USB11] ДОДАНО: enum UsbUiState та поле usbUi (лише в кінець структури) — стан режиму «USB» для мінімального екрана
// (ui/screens::drawUsb). Нуль = Off = режиму немає (екрани Radio / ExternalInput як раніше). Публікує AppController
// (publishState + періодично syncPlayer: носій може зʼявитись / зникнути без події). Без FEATURE_USB_MODE завжди Off.
// [USB17a] ДОДАНО: структура UsbNowPlaying та поле usbNow (лише в кінець AppStateData) — дані «зараз грає» для екрана режиму
// «USB» (малює крок USB17b): трек / виконавець / N з M / час / пауза / shuffle / repeat. Збирає src/usb/usb_now_playing.* з
// ТИХ САМИХ джерел, що й nowPlaying / queue веб-API (usb_queue::status, AudioPlayer), у фіксовані буфери; публікує
// AppController у syncPlayer() (≈ раз на app_controller_cfg::kSyncPeriodMs), стан між потоками — як усе в AppState. Нуль = нічого
// не грає (поза режимом «USB» завжди нуль). Структура без padding, тож однакові дані дають однакові байти для memcmp у screens.
// [Prompt 17] НЕ ВИКОРИСТОВУЮТЬСЯ: ui/screens читає рівні VU напряму з
// VuSourceDecodedPcm::read() (частота кадру), минаючи AppState. Поля лишено, щоб не міняти POD-структуру.

#include <Arduino.h>
#include <stdint.h>

#include "config/eq_config.h"  // [Prompt 49a] eq_cfg::kBandCount для AppStateData::eqGainsDb
#include "config/usb_now_playing_config.h"  // [USB17a] usb_now_playing_cfg::kTextMax для UsbNowPlaying
#include "core/events.h"  // [Prompt 15] ДОДАНО: тип Action

// Режими пристрою.
enum class Mode : uint8_t {
    Standby,
    Radio,
    ExternalInput,
    Menu,
    IrLearn,
    WifiSetup,
    OtaUpdate,  // [Prompt 16] ДОДАНО: запис прошивки, звук зупинено
};

// [Prompt 8] ДОДАНО: яким параметром зараз керує обертання енкодера.
// [Prompt 50b] Лишилися лише Volume і Balance (Bass / Treble / Gain видалено разом із тембром).
enum class AdjustTarget : uint8_t {
    Volume,
    Balance,
};

// [Prompt 8] ДОДАНО: який саме підрежим активний, коли mode == Mode::Menu.
// Нові підрежими (меню налаштувань тощо) додавати лише в кінець.
enum class MenuContext : uint8_t {
    None,
    StationList,
    EqBalance,  // [Prompt 49a] ДОДАНО: екран «EQ + баланс» (Mode::Menu); відтворення / вхід / мʼют не зупиняються
};

// [Prompt 10] ДОДАНО: детальний статус потоку для UI. Незалежно від
// audio_player.h::PlayerState, але семантично відповідає йому.
// Дозволяє ui/screens розрізнити Buffering/Error/Reconnecting без прямого
// доступу до AudioPlayer.
enum class StreamStatus : uint8_t {
    Idle,         // нічого не відтворюється (початковий стан)
    Connecting,   // перша спроба підключення до станції
    Buffering,    // наповнюється буфер або просів під час відтворення
    Playing,      // потік грає
    Error,        // потік обірвався/не підключився
    Reconnecting, // повторна спроба підключення після Error
};

// [USB11] ДОДАНО: стан режиму «USB» для UI (похідний від usb_mode::state() / носія; Off = режиму нема).
enum class UsbUiState : uint8_t {
    Off,       // режиму «USB» нема: показуються екрани Radio / ExternalInput
    Starting,  // іде вхід (радіо / прохід замовкають, піднімається USB-host, чекаємо носій)
    Ready,     // режим активний, носій змонтовано
    NoMedia,   // режим активний, флешки нема (монтування почнеться саме при вставленні)
    Fault,     // носій вставлено, але змонтувати не вдалося
    Stopping,  // іде вихід (зупинка файлу, зняття USB-host, відновлення входу)
    Error,     // збій входу / виходу: стек не знято (AppController сам ініціює «тихий» перезапуск)
};

// [Prompt 15] ДОДАНО: стан навчання IR для UI/вебу. Незалежно від input/ir_rc5.h
// (так само, як StreamStatus від audio_player.h): AppController мапить
// IrRc5::LearnStatus явним switch. Success/Timeout лишаються в AppState і ПІСЛЯ
// повернення з Mode::IrLearn — доки не почнеться нове навчання чи не буде
// cancelIrLearn() — щоб веб-клієнт, який опитує статус, не пропустив результат.
enum class IrLearnStatus : uint8_t {
    Idle,      // навчання не було / скасовано
    Waiting,   // чекаємо перше натискання
    Confirm,   // перший код прийнято, чекаємо такого ж другого
    Success,   // код привʼязано й збережено
    Timeout,   // вичерпано час очікування
    Conflict,  // код уже привʼязаний до іншої дії; чекаємо рішення з вебу
};

// [Prompt 23b] ДОДАНО: розмір буфера назви входу (байт разом із '\0'); має збігатися з
// sizeof(Settings::inputNames[0]) (перевіряє static_assert у app_controller.cpp).
constexpr size_t kInputNameMax = 32;

// [USB17a] ДОДАНО: «зараз грає» в режимі «USB». Порядок полів підібрано так, щоб padding не було (static_assert нижче).
// Не підключає заголовки usb / audio: структуру заповнює usb/usb_now_playing.cpp.
struct UsbNowPlaying {
    uint32_t posSec;     // позиція треку, с (кеш плеєра, роздільність 1 с); 0 — невідомо
    uint32_t durSec;     // тривалість, с; 0 — ще невідомо (екран показує «--:--» і порожню смугу)
    uint16_t count;      // файлів у черзі (знімок папки, на якій запущено відтворення); 0 — черги немає
    uint16_t pos;        // 1-based позиція треку в ПОТОЧНОМУ порядку проходу (usb_queue::Status::pos); 0 — невідомо
    bool active;         // трек стартує / грає / на паузі (черга не Idle); інакше всі «трекові» поля нульові
    bool paused;         // файл на паузі
    bool shuffle;        // перемішування (дійсне й коли active == false, поки режим «USB» у Ready / Playing)
    bool repeatAll;      // повтор усього (так само)
    // Текст УЖЕ нормалізований під шрифти дисплея (usb/usb_np_text.h): UTF-8, лише символи, що має шрифт; хвіст обнулено.
    char title[usb_now_playing_cfg::kTextMax];    // тег title, інакше імʼя файла без розширення; порожньо, якщо active == false
    char artist[usb_now_playing_cfg::kTextMax];   // тег artist; порожньо, якщо тегу нема
};
static_assert(sizeof(UsbNowPlaying) == 16 + 2 * usb_now_playing_cfg::kTextMax,
              "UsbNowPlaying must have no padding (memcmp of AppStateData snapshots)");

// Дані стану.
// Розміри буферів під метадані залишаються константами.
struct AppStateData {
    Mode mode;
    uint8_t inputIndex;

    int8_t volume;   // ЦІЛЬОВА гучність (під час ramp в чіпі може бути менше)
    bool mute;       // мʼют, який зробив користувач (Standby/переходи сюди не входять)

    uint16_t stationIndex;
    char stationName[64];
    char trackTitle[128];

    bool wifiConnected;
    bool streamPlaying;

    // Рівні VU, нормалізовані 0..1.
    float vuLeft;
    float vuRight;

    // --- [Prompt 8] ДОДАНО (лише в кінець) ---
    // Ціль, яку регулює обертання енкодера.
    AdjustTarget adjustTarget;
    // Підрежим меню; осмислений лише при mode == Mode::Menu (інакше None).
    MenuContext menuContext;
    // Вибраний пункт у меню/списку (для StationList — індекс станції).
    uint16_t menuSelection;

    // --- [Prompt 10] ДОДАНО ---
    // Детальний статус потоку для UI (Idle/Connecting/Buffering/Playing/Error/Reconnecting).
    StreamStatus streamStatus;

    // --- [Prompt 12] ДОДАНО ---
    // Поточна Wi-Fi мережа (STA) або назва точки доступу (AP); "" якщо невідомо.
    char wifiSsid[33];
    // IP: отриманий (STA) або адреса AP ("192.168.4.1"); "" якщо немає.
    char wifiIp[16];
    // true: пристрій у режимі AP (captive portal), STA не підключена.
    bool wifiApMode;

    // --- [Prompt 15] ДОДАНО ---
    // Дія, яку навчають; осмислена, коли irLearnStatus != Idle.
    Action irLearnTarget;
    IrLearnStatus irLearnStatus;
    // З якою дією конфлікт кодів; осмислене лише при irLearnStatus == Conflict.
    Action irLearnConflictWith;

    // --- [Prompt 16] ДОДАНО ---
    // Прогрес OTA 0..100; осмислений лише при mode == Mode::OtaUpdate.
    uint8_t otaProgress;

    // --- [Prompt 23b] ДОДАНО ---
    // Назва поточного входу (UTF-8, завжди з '\0'; хвіст буфера обнулено, щоб однакові
    // назви давали однакові байти для memcmp у screens). "" лише якщо немає ні користувацької,
    // ні типової назви.
    char inputName[kInputNameMax];

    // --- [Prompt 28] ДОДАНО ---
    // true: офлайн-режим (Wi-Fi вимкнено, вхід Radio недоступний). Лише AppController пише.
    bool offline;
    // true: триває «тихий» перезапуск (мʼют -> підсилювач LOW -> ESP.restart()).
    bool restarting;

    // --- [Prompt 32] ДОДАНО ---
    // Параметр, який показує спливне вікно (осмислений, коли popupSeq змінився).
    AdjustTarget popupTarget;
    // Лічильник подій «показати вікно»; UI порівнює з попереднім баченим значенням.
    uint8_t popupSeq;

    // --- [Prompt 36] ДОДАНО ---
    // RSSI поточної STA-мережі, дБм (від'ємне, кратне kRssiQuantDb); 0 — невідомо або не підключено.
    int8_t wifiRssi;

    // --- [Prompt 39] ДОДАНО ---
    // Маска дозволених користувачем входів (біт i = вхід i; біт 0 завжди 1). Лише AppController пише.
    uint8_t inputEnabledMask;

    // --- [Prompt 48b] ДОДАНО ---
    // Баланс АКТИВНОГО входу, дБ (balance_cfg::kMinDb..kMaxDb; додатне = вправо: лівий канал послаблено,
    // 0 = без змін). Лише AppController пише. Старе поле `balance` (legacy, завжди 0) не чіпається — прибере P50.
    int8_t balanceDb;

    // --- [Prompt 49a] ДОДАНО ---
    // Екран «EQ + баланс» (осмислені, лише коли mode == Mode::Menu && menuContext == MenuContext::EqBalance;
    // інакше 0). eqSelected: 0..eq_cfg::kBandCount - 1 = смуга EQ, eq_cfg::kBandCount = баланс (його значення — balanceDb).
    // eqGainsDb = пресет EQ АКТИВНОГО входу, дБ (eq_cfg::kGainMinDb..kGainMaxDb). Лише AppController пише.
    uint8_t eqSelected;
    int8_t eqGainsDb[eq_cfg::kBandCount];

    // --- [Prompt 49d] ДОДАНО (в кінець структури) ---
    // true: спливне вікно (гучність / баланс) треба сховати негайно — користувач натиснув кнопку / клікнув
    // енкодер / натиснув кнопку IR (крім дій, що самі показують вікно). Це РІВЕНЬ, а не подія: UI не плутає
    // порядок «показати» / «сховати» в межах одного кадру. Скидається в false, коли AppController знову
    // показує вікно (notifyPopup). Лише AppController пише.
    bool popupDismissed;

    // --- [V1] ДОДАНО (в кінець структури) ---
    // Чутливість VU активного входу, дБ (vu_sens_cfg::kMinDb..kMaxDb; 0 = без зсуву). Лише AppController пише.
    // Сам VU зсув застосовує audio/vu_source (VuSource::setSensitivityDb); поле — для веб-статусу й діагностики.
    int8_t vuSensDb;

    // --- [USB11] ДОДАНО (в кінець структури) ---
    // Стан режиму «USB» для мінімального екрана (UsbUiState::Off = режиму нема). Лише AppController пише.
    UsbUiState usbUi;

    // --- [USB17a] ДОДАНО (в кінець структури) ---
    // Дані «зараз грає» режиму «USB» (UsbNowPlaying; нуль = нічого не грає / режиму нема). Лише AppController пише (syncPlayer).
    UsbNowPlaying usbNow;
};

class AppState {
public:
    // Створює мʼютекс і заповнює стан значеннями з config/defaults.h
    // (mode = Standby). Повторні виклики нічого не скидають.
    static bool begin();

    // Потокобезпечна копія стану.
    static AppStateData snapshot();

    // Потокобезпечна ПОВНА заміна стану. Залишено для сумісності.
    // Патерн «snapshot() → змінити поле → update()» НЕ атомарний: паралельна
    // зміна іншого поля іншою задачею (наприклад, VU з audio-ядра) буде
    // затерта. Для зміни окремих полів користуйтеся modify().
    static void update(const AppStateData& data);

    // [Prompt 6] ДОДАНО: атомарна часткова зміна. fn викликається під мʼютексом
    // над внутрішнім станом і міняє лише потрібні поля. fn має бути короткою й
    // не викликати AppState / Serial / інші блокуючі функції.
    // fn — звичайна функція або лямбда без захоплень (вказівник не має контексту;
    // для передачі значення див. перевантаження нижче).
    static void modify(void (*fn)(AppStateData&));

    // [Prompt 6] ДОДАНО: те саме з довільним контекстом, напр.
    //   int8_t v = 30;
    //   AppState::modify([](AppStateData& s, void* c) { s.volume = *static_cast<int8_t*>(c); }, &v);
    static void modify(void (*fn)(AppStateData&, void* ctx), void* ctx);
};
