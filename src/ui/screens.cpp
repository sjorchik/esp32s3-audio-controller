#include "ui/screens.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

#include "config/defaults.h"
#include "config/display_config.h"
#include "config/screens_config.h"
#include "audio/vu_source.h"      // [Prompt 17] ДОДАНО: рівні VU (читаються напряму)
#include "config/features.h"
#include "core/action_names.h"  // [Prompt 15] ДОДАНО: імена дій (чисті дані)
#include "core/app_state.h"
#include "ui/display.h"
#include "ui/fonts.h"
#include "stations/station_store.h"  // [Prompt 11] ДОДАНО: чисті дані, не залізо
#include "ui/icons.h"

#if DISPLAY_DEMO
#warning "DISPLAY_DEMO = 1: постав 0 у config/display_config.h, щоб показувати реальні екрани"
#endif

// [Prompt 11] ВИНЯТОК З ІЗОЛЯЦІЇ: окрім AppState::snapshot(), цей файл читає
// UiFonts/UiIcons і StationStore::count()/get() (список станцій потрібен одразу
// для кількох сусідніх рядків). StationStore — модуль чистих даних, не залізо,
// тож правило «AppController — єдиний власник заліза» не порушується.
// [Prompt 15] Так само читається core/action_names (таблиця імен без стану); стан навчання
// IR береться з AppState (irLearnStatus/irLearnTarget/irLearnConflictWith), не з IrRc5.
// [Prompt 16] Екран Mode::OtaUpdate читає лише AppState.otaProgress.
// [Prompt 23b] Назва входу (лише ExternalInput; з Radio її прибрано в P33) береться з
// AppState.inputName; Settings екрани не читають. Довга назва обрізається fitText() з "..."
// по межі UTF-8-символу.
// [Prompt 28] AppState.offline: замість Wi-Fi-індикатора у верхній панелі — текст "offline" (Tiny);
// на екрані WifiSetup — підказка "OK: work offline"; AppState.restarting — екран "Restarting...".
// [Prompt 32] Спливне вікно параметра: UI веде ЛИШЕ відлік показу (millis() від моменту, коли
// AppState.popupSeq змінився); сам параметр і його значення читаються зі знімка AppState.
// Статус-рядок з ціллю енкодера (drawStatusRow) прибрано.
// [Prompt 33] Екран Radio: назву входу прибрано; рядок метаданих і рядок статусу потоку обʼєднано
// в один (слот kTrackY, marquee s_track): метадані, коли Playing і вони непорожні, інакше статус.
// [Prompt 17] ВИНЯТОК З ІЗОЛЯЦІЇ: рівні VU беруться напряму з VuSourceDecodedPcm::read() (з
// частотою кадру), МИНАЮЧИ AppState; AppStateData.vuLeft/vuRight не використовуються.

namespace {

using D = DisplayManager;
namespace c = screens_cfg;
namespace dc = display_cfg;

static_assert(std::is_trivially_copyable<AppStateData>::value,
              "AppStateData має бути POD для memcmp");

// ---------------------------------------------------------------------------
// Стан UI (лише візуальний, не частина AppStateData)
// ---------------------------------------------------------------------------
struct Marquee {
    char     text[128];
    int32_t  textW;
    int32_t  offset;
    uint32_t lastStepMs;
    uint32_t holdUntilMs;
    bool     atEnd;
    bool     seeded;
};

Marquee s_station = {};
Marquee s_track   = {};   // [Prompt 33] обʼєднаний рядок «метадані / статус»

// [Prompt 33] Остання побачена станція: зміна індексу скидає s_track на початок, навіть якщо
// текст рядка (напр. статус) лишився тим самим.
bool     s_trackStationKnown = false;
uint16_t s_trackStation      = 0;

// [Prompt 17] VU: джерело (читає лише display-задача) і кількість засвічених сегментів
// минулого кадру (перемальовуємо, лише коли вона змінилась).
VuSourceDecodedPcm s_vu;
int s_vuLitL = 0;
int s_vuLitR = 0;

// [Prompt 32] Спливне вікно: останній побачений popupSeq і час закінчення показу.
bool     s_popupSeqKnown = false;
uint8_t  s_popupSeqSeen  = 0;
bool     s_popupActive   = false;
uint32_t s_popupUntilMs  = 0;

AppStateData s_prev;            // попередній знімок
bool         s_havePrev = false;
bool         s_modeKnown = false;
Mode         s_prevMode = Mode::Standby;
uint8_t      s_savedBrightness = defaults::kDefaultBrightness;

// ---------------------------------------------------------------------------
// Marquee
// ---------------------------------------------------------------------------
// Повертає true, якщо вигляд змінився (новий текст або зсув) і потрібен кадр.
bool marqueeUpdate(Marquee& m, const char* src, FontSize size, uint32_t now) {
    if (!m.seeded || strcmp(m.text, src) != 0) {
        strlcpy(m.text, src, sizeof(m.text));
        m.textW       = m.text[0] ? UiFonts::textWidth(m.text, size) : 0;
        m.offset      = 0;
        m.lastStepMs  = now;
        m.holdUntilMs = now + c::kMarqueeEdgePauseMs;
        m.atEnd       = false;
        m.seeded      = true;
        return true;
    }
    if (m.textW <= c::kContentW) return false;

    if (static_cast<int32_t>(now - m.holdUntilMs) < 0) {   // пауза
        m.lastStepMs = now;
        return false;
    }
    if (m.atEnd) {                                          // кінець -> початок
        m.offset      = 0;
        m.atEnd       = false;
        m.holdUntilMs = now + c::kMarqueeEdgePauseMs;
        m.lastStepMs  = now;
        return true;
    }
    const uint32_t steps = (now - m.lastStepMs) / c::kMarqueeStepMs;
    if (steps == 0) return false;
    m.lastStepMs += steps * c::kMarqueeStepMs;
    m.offset += static_cast<int32_t>(steps) * c::kMarqueeStepPx;
    const int32_t maxOff = m.textW - c::kContentW;
    if (m.offset >= maxOff) {
        m.offset      = maxOff;
        m.atEnd       = true;
        m.holdUntilMs = now + c::kMarqueeEdgePauseMs;
    }
    return true;
}

// DisplayManager не має clip-прямокутника: текст малюється зі зсувом, а потім
// бічні поля перекриваються кольором фону. Тому рядок marquee має займати
// всю ширину екрана без іншого вмісту.
void marqueeDraw(const Marquee& m, int16_t y, FontSize size, uint16_t color) {
    if (!m.text[0]) return;
    const bool scrolling = m.textW > c::kContentW;
    const int16_t x = static_cast<int16_t>(c::kMargin - (scrolling ? m.offset : 0));
    D::drawText(m.text, x, y, size, color);
    if (scrolling) {
        const int16_t h = static_cast<int16_t>(UiFonts::lineHeight(size));
        D::fillRect(0, y, c::kMargin, h, dc::kColorBg);
        D::fillRect(c::kW - c::kMargin, y, c::kMargin, h, dc::kColorBg);
    }
}

// ---------------------------------------------------------------------------
// Допоміжні малювання
// ---------------------------------------------------------------------------
void drawCentered(const char* text, int16_t y, FontSize size, uint16_t color) {
    int32_t x = (c::kW - UiFonts::textWidth(text, size)) / 2;
    if (x < 0) x = 0;
    D::drawText(text, static_cast<int16_t>(x), y, size, color);
}

// Вписує src у maxW пікселів: за потреби обрізає по межі UTF-8-символу й додає
// "...". out має вміщати кінцевий текст: cap >= sizeof(Station::name) + 8.
void fitText(const char* src, char* out, size_t cap, FontSize size, int32_t maxW) {
    strlcpy(out, src, cap);
    if (UiFonts::textWidth(out, size) <= maxW) return;
    size_t len = strlen(src);
    while (len > 0) {
        --len;
        while (len > 0 && (static_cast<uint8_t>(src[len]) & 0xC0) == 0x80) --len;
        memcpy(out, src, len);
        strcpy(out + len, "...");
        if (UiFonts::textWidth(out, size) <= maxW) return;
    }
}

// [Prompt 23b] Назва входу береться з AppState.inputName (її вирішує AppController: користувацька
// з налаштувань, запасно defaults::kInputNames). Тут лише останній запасний варіант на випадок
// порожнього поля (нульовий AppState до першої публікації): типова назва за індексом або "Input".
// [Prompt 33] Використовується лише екраном ExternalInput.
const char* inputName(const AppStateData& s) {
    if (s.inputName[0] != '\0') return s.inputName;
    return (s.inputIndex < defaults::kInputCount) ? defaults::kInputNames[s.inputIndex] : "Input";
}

// Буфер під fitText для назви входу: cap >= sizeof(inputName) + 8.
constexpr size_t kInputFitCap = kInputNameMax + 8;

void drawTopIcons(const AppStateData& s, bool showWifi) {
    int16_t muteX = c::kMuteIconX;
    if (s.offline) {
        // [Prompt 28] Офлайн: у слоті Wi-Fi-індикатора — текст "offline" (нової іконки немає);
        // іконка мʼюту зсувається ліворуч від нього. Показуємо і там, де Wi-Fi-іконки не було
        // (ExternalInput), бо в офлайні це єдиний екран зі верхньою панеллю.
        constexpr const char* kOfflineText = "offline";
        const int32_t w = UiFonts::textWidth(kOfflineText, FontSize::Tiny);
        const int16_t x = static_cast<int16_t>(c::kW - c::kMargin - w);
        const int16_t h = static_cast<int16_t>(UiFonts::lineHeight(FontSize::Tiny));
        const int16_t y = static_cast<int16_t>(c::kTopBarY + (dc::kIconSize - h) / 2);
        D::drawText(kOfflineText, x, y, FontSize::Tiny, c::kColorAccent);
        muteX = static_cast<int16_t>(x - c::kOfflineMuteGap - dc::kIconSize);
    } else if (showWifi) {
        D::drawIcon(s.wifiConnected ? IconId::Wifi : IconId::WifiOff, c::kWifiIconX, c::kTopBarY,
                    s.wifiConnected ? dc::kColorFg : c::kColorBad);
    }
    if (s.mute) D::drawIcon(IconId::Mute, muteX, c::kTopBarY, c::kColorAccent);
}

// [Prompt 17] VU-метр: дві сегментні смуги L (верхня) / R (нижня).
// ENABLE_VU == 0: стара порожня рамка. Без Tiny-підписів L/R — тоді смуги довелося б
// звужувати, а порядок «верх = L» очевидний і так.
#if ENABLE_VU
int vuToSegments(float level) {
    int n = static_cast<int>(level * c::kVuSegments + 0.5f);
    if (n < 0) n = 0;
    if (n > c::kVuSegments) n = c::kVuSegments;
    return n;
}

void drawVuBar(int16_t y, int lit) {
    for (uint8_t i = 0; i < c::kVuSegments; ++i) {
        uint16_t color;
        if (i >= lit)                      color = c::kColorVuOff;
        else if (i >= c::kVuRedSeg)        color = c::kColorBad;
        else if (i >= c::kVuYellowSeg)     color = c::kColorAccent;
        else                               color = c::kColorOk;
        const int16_t x = static_cast<int16_t>(c::kVuBarX + i * (c::kVuSegW + c::kVuSegGap));
        D::fillRect(x, y, c::kVuSegW, c::kVuBarH, color);
    }
}
#endif

void drawVu(int litL, int litR) {
#if ENABLE_VU
    drawVuBar(c::kVuY, litL);
    drawVuBar(static_cast<int16_t>(c::kVuY + c::kVuBarH + c::kVuBarGap), litR);
#else
    (void)litL;
    (void)litR;
    D::drawRect(c::kVuX, c::kVuY, c::kVuW, c::kVuH, c::kColorDim);
#endif
}

// [Prompt 32] Спливне вікно параметра звуку: підпис (Large) + значення великими цифрами.
// Знак +/- лише для параметрів зі знаком (тембр, баланс), і лише коли значення не нуль
// (як було в статус-рядку). Гучність і gain — без знака.
void drawPopup(const AppStateData& s) {
    const char* label = "";
    int value = 0;
    bool signedValue = false;
    switch (s.popupTarget) {
        case AdjustTarget::Volume:  label = "Volume";    value = s.volume;  break;
        case AdjustTarget::Bass:    label = "Bass";      value = s.bass;    signedValue = true; break;
        case AdjustTarget::Treble:  label = "Treble";    value = s.treble;  signedValue = true; break;
        case AdjustTarget::Balance: label = "Balance";   value = s.balance; signedValue = true; break;
        case AdjustTarget::Gain:    label = "Gain";      value = s.gain;    break;
    }
    char buf[8];
    if (signedValue && value != 0) snprintf(buf, sizeof(buf), "%+d", value);
    else                           snprintf(buf, sizeof(buf), "%d", value);

    D::fillRect(c::kPopupX, c::kPopupY, c::kPopupW, c::kPopupH, c::kColorPopupBorder);
    D::fillRect(static_cast<int16_t>(c::kPopupX + c::kPopupBorder),
                static_cast<int16_t>(c::kPopupY + c::kPopupBorder),
                static_cast<int16_t>(c::kPopupW - 2 * c::kPopupBorder),
                static_cast<int16_t>(c::kPopupH - 2 * c::kPopupBorder), c::kColorPopupBg);

    const int32_t lw = UiFonts::textWidth(label, FontSize::Large);
    D::drawText(label, static_cast<int16_t>(c::kPopupX + (c::kPopupW - lw) / 2), c::kPopupLabelY,
                FontSize::Large, c::kColorPopupLabel);
    const int32_t dw = UiFonts::textWidth(buf, FontSize::Digits);
    D::drawText(buf, static_cast<int16_t>(c::kPopupX + (c::kPopupW - dw) / 2), c::kPopupDigitsY,
                FontSize::Digits, c::kColorPopupDigits);
}

// [Prompt 10] Текст і колір статусу потоку за StreamStatus.
// [Prompt 33] Раніше drawStreamStatus() малювала це окремим рядком (y=122); тепер це лише
// вибір тексту й кольору (тексти БЕЗ змін), а малює обʼєднаний рядок drawRadio().
//   Idle → сірий "Stopped"; Connecting/Buffering/Reconnecting → жовті;
//   Playing → зелений "Playing"; Error → червоний "Error".
// Wi-Fi статус показується окремою іконкою (не змінюється).
const char* statusText(StreamStatus st, uint16_t& color) {
    switch (st) {
        case StreamStatus::Idle:
            color = c::kColorDim;
            return "Stopped";
        case StreamStatus::Connecting:
            color = c::kColorAccent;
            return "Connecting…";
        case StreamStatus::Buffering:
            color = c::kColorAccent;
            return "Buffering…";
        case StreamStatus::Playing:
            color = c::kColorOk;
            return "Playing";
        case StreamStatus::Error:
            color = c::kColorBad;
            return "Error";
        case StreamStatus::Reconnecting:
            color = c::kColorAccent;
            return "Reconnecting…";
        default:
            color = c::kColorDim;
            return "?";
    }
}

// [Prompt 33] Обʼєднаний рядок екрана Radio: метадані, коли потік грає й вони є; інакше статус.
// Повертає вказівник або на s.trackTitle, або на літерал; color — колір відповідного тексту.
const char* radioInfoLine(const AppStateData& s, uint16_t& color) {
    if (s.streamStatus == StreamStatus::Playing && s.trackTitle[0] != '\0') {
        color = c::kColorDim;   // як було в рядка метаданих
        return s.trackTitle;
    }
    return statusText(s.streamStatus, color);
}

// ---------------------------------------------------------------------------
// Екрани
// ---------------------------------------------------------------------------
void drawStandby() {
    D::drawIcon(IconId::Standby, c::kStandbyIconX, c::kStandbyIconY, c::kColorDim);
    drawCentered("Standby", c::kStandbyCaptionY, FontSize::Tiny, c::kColorDim);
}

void drawRadio(const AppStateData& s, int vuLitL, int vuLitR) {
    // [Prompt 33] Назву входу прибрано; лишились лише іконки верхньої панелі.
    drawTopIcons(s, true);

    marqueeDraw(s_station, c::kStationY, FontSize::Large, dc::kColorFg);

    // [Prompt 33] Обʼєднаний рядок «метадані / статус» (текст у s_track оновлює frame()).
    uint16_t infoColor = c::kColorDim;
    (void)radioInfoLine(s, infoColor);
    marqueeDraw(s_track, c::kTrackY, FontSize::Small, infoColor);

    drawVu(vuLitL, vuLitR);   // [Prompt 17]
}

void drawExternal(const AppStateData& s) {
    drawTopIcons(s, false);
    drawCentered("INPUT", c::kExtCaptionY, FontSize::Tiny, c::kColorDim);
    char name[kInputFitCap];  // [Prompt 23b]
    fitText(inputName(s), name, sizeof(name), FontSize::Large, c::kContentW);
    drawCentered(name, c::kExtNameY, FontSize::Large, dc::kColorFg);
}

void drawStationList(const AppStateData& s) {
    D::drawText("Stations", c::kMargin, c::kListTitleY, FontSize::Small, c::kColorDim);
    D::drawLine(0, c::kListLineY, c::kW - 1, c::kListLineY, c::kColorDim);

    // [Prompt 11] Реальні назви зі StationStore.
    const size_t count = StationStore::count();
    if (count == 0) {
        drawCentered("No stations", c::kPlaceholderY, FontSize::Large, c::kColorDim);
        return;
    }

    const int32_t maxW = c::kW - c::kListTextX - c::kMargin;
    const uint16_t sel = s.menuSelection;
    const uint16_t first = (sel >= c::kListRows) ? static_cast<uint16_t>(sel - c::kListRows + 1) : 0;
    Station st;
    char text[sizeof(Station::name) + 8];
    for (uint8_t i = 0; i < c::kListRows; ++i) {
        const uint32_t idx = static_cast<uint32_t>(first) + i;
        const int16_t y = static_cast<int16_t>(c::kListFirstY + i * c::kListRowH);
        if (idx == sel) D::fillRect(0, y, c::kW, c::kListRowH, c::kColorSelectionBg);
        if (idx >= count || !StationStore::get(idx, st)) continue;  // порожній рядок
        fitText(st.name, text, sizeof(text), FontSize::Small, maxW);
        D::drawText(text, c::kListTextX, static_cast<int16_t>(y + c::kListTextDy), FontSize::Small,
                    dc::kColorFg);
    }
}

void drawPlaceholder(const char* text) {
    drawCentered(text, c::kPlaceholderY, FontSize::Large, dc::kColorFg);
}

// [Prompt 12] Екран Mode::WifiSetup: до чого підключитись і що відкрити на телефоні.
void drawWifiSetup(const AppStateData& s) {
    drawCentered("Setup Wi-Fi", c::kWifiSetupTitleY, FontSize::Large, dc::kColorFg);
    drawCentered(s.wifiSsid[0] ? s.wifiSsid : defaults::kApSsid, c::kWifiSetupSsidY,
                 FontSize::Small, c::kColorAccent);
    char url[32];
    snprintf(url, sizeof(url), "http://%s", s.wifiIp[0] ? s.wifiIp : "192.168.4.1");
    drawCentered(url, c::kWifiSetupIpY, FontSize::Small, c::kColorDim);
    // [Prompt 28] Підказка: OK (кнопка / енкодер / IR) запускає офлайн-режим.
    drawCentered("OK: work offline", c::kWifiSetupHintY, FontSize::Small, c::kColorDim);
}

// [Prompt 15] Екран Mode::IrLearn: статус (Small) над назвою дії (Large), підказка під нею.
// Лише інформаційний: підтвердження/скасування робиться з вебу. Статус «Waiting» дає
// «Press remote button for» просто над назвою дії - фраза читається разом з нею.
void drawIrLearn(const AppStateData& s) {
    char line[40];
    const char* hint = "";
    uint16_t color = c::kColorDim;

    switch (s.irLearnStatus) {
        case IrLearnStatus::Waiting:
            strlcpy(line, "Press remote button for", sizeof(line));
            color = c::kColorAccent;
            break;
        case IrLearnStatus::Confirm:
            strlcpy(line, "Press again to confirm", sizeof(line));
            color = c::kColorAccent;
            break;
        case IrLearnStatus::Success:
            strlcpy(line, "Learned!", sizeof(line));
            color = c::kColorOk;
            break;
        case IrLearnStatus::Timeout:
            strlcpy(line, "Timeout", sizeof(line));
            color = c::kColorBad;
            break;
        case IrLearnStatus::Conflict:
            snprintf(line, sizeof(line), "Already used by %s",
                     action_names::name(s.irLearnConflictWith));
            hint = "Resolve on web";
            color = c::kColorBad;
            break;
        case IrLearnStatus::Idle:
        default:
            line[0] = '\0';  // кадр-два між зміною режиму й першим статусом
            break;
    }

    char text[48];  // fitText: cap >= довжина джерела + 8
    fitText(line, text, sizeof(text), FontSize::Small, c::kContentW);
    drawCentered(text, c::kIrStatusY, FontSize::Small, color);
    drawCentered(action_names::name(s.irLearnTarget), c::kIrActionY, FontSize::Large,
                 dc::kColorFg);
    if (hint[0] != '\0') {
        drawCentered(hint, c::kIrHintY, FontSize::Small, c::kColorDim);
    }
}

// [Prompt 16] Екран Mode::OtaUpdate: великий відсоток по центру + смуга; під час прошивки
// дивляться на пристрій, а не на телефон. Три крапки ASCII, не «…»: U+2026 немає в шрифті
// (ASCII/Latin-1/Latin Ext-A/кирилиця).
void drawOtaUpdate(const AppStateData& s) {
    const uint8_t pct = s.otaProgress > 100 ? 100 : s.otaProgress;
    drawCentered("Updating...", c::kOtaTitleY, FontSize::Large, dc::kColorFg);
    char num[8];
    snprintf(num, sizeof(num), "%u%%", static_cast<unsigned>(pct));
    drawCentered(num, c::kOtaPercentY, FontSize::Large, c::kColorAccent);
    D::drawRect(c::kOtaBarX, c::kOtaBarY, c::kOtaBarW, c::kOtaBarH, c::kColorDim);
    const int16_t inner = static_cast<int16_t>(c::kOtaBarW - 2);
    const int16_t fillW = static_cast<int16_t>((static_cast<int32_t>(inner) * pct) / 100);
    if (fillW > 0) {
        D::fillRect(c::kOtaBarX + 1, c::kOtaBarY + 1, fillW, c::kOtaBarH - 2, c::kColorAccent);
    }
    drawCentered("Do not power off", c::kOtaHintY, FontSize::Small, c::kColorDim);
}

// ---------------------------------------------------------------------------
// Підсвітка при вході/виході зі Standby
// ---------------------------------------------------------------------------
void handleModeChange(Mode mode) {
    if (s_modeKnown && mode == s_prevMode) return;

    const bool wasStandby = s_modeKnown && s_prevMode == Mode::Standby;
    const bool isStandby  = mode == Mode::Standby;
    if (isStandby && !wasStandby) {
        s_savedBrightness = D::brightness();
        D::setBrightness(c::kStandbyBrightnessPercent);
    } else if (wasStandby && !isStandby) {
        D::setBrightness(s_savedBrightness);
    }
    // Нова сцена -> marquee починається спочатку.
    s_station.seeded = false;
    s_track.seeded   = false;
    s_prevMode  = mode;
    s_modeKnown = true;
}

// ---------------------------------------------------------------------------
// FrameCallback
// ---------------------------------------------------------------------------
bool frame() {
    const uint32_t now = millis();
    const AppStateData s = AppState::snapshot();   // один знімок на кадр

    handleModeChange(s.mode);

    bool dirty = !s_havePrev || memcmp(&s, &s_prev, sizeof(s)) != 0;
    if (dirty) {
        s_prev = s;
        s_havePrev = true;
    }

    // [Prompt 17] VU: лише Radio + Playing, інакше рівні 0. read() == false (даних
    // нема) теж дає 0. Перемальовуємо, коли змінилась кількість засвічених сегментів
    // (під час музики це практично кожен кадр; у тиші чи на паузі екран знову статичний).
    int litL = 0;
    int litR = 0;
#if ENABLE_VU
    if (s.mode == Mode::Radio && s.streamStatus == StreamStatus::Playing) {
        float vuL = 0.0f;
        float vuR = 0.0f;
        if (s_vu.read(vuL, vuR)) {
            litL = vuToSegments(vuL);
            litR = vuToSegments(vuR);
        }
    }
#endif
    if (litL != s_vuLitL || litR != s_vuLitR) {
        dirty = true;
        s_vuLitL = litL;
        s_vuLitR = litR;
    }

    // [Prompt 32] Спливне вікно: нова подія (popupSeq змінився) запускає/продовжує показ;
    // поза Radio/ExternalInput (і під час перезапуску) вікно ховається й не повертається саме.
    const bool popupMode = (s.mode == Mode::Radio || s.mode == Mode::ExternalInput) && !s.restarting;
    if (!s_popupSeqKnown) {
        s_popupSeqKnown = true;                 // перший кадр: не вважати старе значення подією
        s_popupSeqSeen  = s.popupSeq;
    } else if (s.popupSeq != s_popupSeqSeen) {
        s_popupSeqSeen = s.popupSeq;
        if (popupMode) {
            s_popupActive  = true;
            s_popupUntilMs = now + c::kPopupTimeoutMs;
        }
    }
    if (!popupMode) {
        s_popupActive = false;
    } else if (s_popupActive && static_cast<int32_t>(now - s_popupUntilMs) >= 0) {
        s_popupActive = false;
        dirty = true;                           // перемалювати кадр без вікна
    }

    if (s.mode == Mode::Radio) {
        dirty |= marqueeUpdate(s_station, s.stationName[0] ? s.stationName : "No station",
                               FontSize::Large, now);

        // [Prompt 33] Зміна станції — рядок «метадані / статус» починається спочатку (навіть якщо
        // текст не змінився). Зміну самого тексту (метадані ↔ статус, нові метадані) marqueeUpdate
        // ловить сам через strcmp.
        if (!s_trackStationKnown || s.stationIndex != s_trackStation) {
            s_trackStationKnown = true;
            s_trackStation      = s.stationIndex;
            s_track.seeded      = false;
        }
        uint16_t infoColor = c::kColorDim;   // колір тут не потрібен, лише текст
        const char* info = radioInfoLine(s, infoColor);
        dirty |= marqueeUpdate(s_track, info, FontSize::Small, now);
    }

    if (!dirty) return false;   // панель лишається як була

    D::fillScreen(dc::kColorBg);
    if (s.restarting) {  // [Prompt 28] «тихий» перезапуск: один рядок замість будь-якого екрана
        drawPlaceholder("Restarting...");  // три крапки ASCII: U+2026 немає у шрифті
        return true;
    }
    switch (s.mode) {
        case Mode::Standby:       drawStandby();                 break;
        case Mode::Radio:         drawRadio(s, litL, litR);      break;
        case Mode::ExternalInput: drawExternal(s);               break;
        case Mode::Menu:
            if (s.menuContext == MenuContext::StationList) drawStationList(s);
            else                                           drawPlaceholder("Menu (TODO)");
            break;
        case Mode::IrLearn:       drawIrLearn(s);                break;   // [Prompt 15]
        case Mode::WifiSetup:     drawWifiSetup(s);              break;
        case Mode::OtaUpdate:     drawOtaUpdate(s);              break;   // [Prompt 16]
        default:                  drawPlaceholder("?");          break;
    }
    if (s_popupActive) drawPopup(s);   // [Prompt 32] останній шар кадру
    return true;
}

}  // namespace

bool UiScreens::begin() {
    s_havePrev  = false;
    s_modeKnown = false;
    s_station.seeded = false;
    s_track.seeded   = false;
    s_trackStationKnown = false;   // [Prompt 33]
    s_vuLitL = 0;
    s_vuLitR = 0;
    s_popupSeqKnown = false;   // [Prompt 32]
    s_popupActive   = false;
    DisplayManager::setFrameCallback(&frame);
    return true;
}