#include "ui/screens.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

#include "config/defaults.h"
#include "config/display_config.h"
#include "config/screens_config.h"
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
Marquee s_track   = {};

AppStateData s_prev;            // попередній знімок (VU обнулено)
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

const char* inputName(uint8_t index) {
    return (index < defaults::kInputCount) ? defaults::kInputNames[index] : "Input";
}

void drawTopIcons(const AppStateData& s, bool showWifi) {
    if (showWifi) {
        D::drawIcon(s.wifiConnected ? IconId::Wifi : IconId::WifiOff, c::kWifiIconX, c::kTopBarY,
                    s.wifiConnected ? dc::kColorFg : c::kColorBad);
    }
    if (s.mute) D::drawIcon(IconId::Mute, c::kMuteIconX, c::kTopBarY, c::kColorAccent);
}

// Статус-рядок: ціль енкодера зліва, значення справа. Завжди видимий.
void drawStatusRow(const AppStateData& s) {
    const char* label = "";
    int value = 0;
    bool signedValue = false;
    switch (s.adjustTarget) {
        case AdjustTarget::Volume:  label = "Volume";  value = s.volume;  break;
        case AdjustTarget::Bass:    label = "Bass";    value = s.bass;    signedValue = true; break;
        case AdjustTarget::Treble:  label = "Treble";  value = s.treble;  signedValue = true; break;
        case AdjustTarget::Balance: label = "Balance"; value = s.balance; signedValue = true; break;
        case AdjustTarget::Gain:    label = "Gain";    value = s.gain;    break;
    }
    char buf[12];
    if (signedValue && value != 0) snprintf(buf, sizeof(buf), "%+d", value);
    else                           snprintf(buf, sizeof(buf), "%d", value);

    D::drawLine(0, c::kStatusLineY, c::kW - 1, c::kStatusLineY, c::kColorDim);
    D::drawText(label, c::kMargin, c::kStatusY, FontSize::Small, dc::kColorFg);
    const int32_t w = UiFonts::textWidth(buf, FontSize::Small);
    D::drawText(buf, static_cast<int16_t>(c::kW - c::kMargin - w), c::kStatusY, FontSize::Small,
                c::kColorAccent);
}

// [Prompt 10] ДОДАНО: малювання статусу потоку на основі StreamStatus.
// Виводить текст і колір залежно від статусу:
//   Idle           → сірий "Stopped"
//   Connecting     → жовтий "Connecting…"
//   Buffering      → жовтий "Buffering…"
//   Playing        → зелений "Playing"
//   Error          → червоний "Error"
//   Reconnecting   → жовтий "Reconnecting…"
// Wi-Fi статус показується окремою іконкою (не змінюється).
void drawStreamStatus(const AppStateData& s) {
    const char* state;
    uint16_t color;

    switch (s.streamStatus) {
        case StreamStatus::Idle:
            state = "Stopped";
            color = c::kColorDim;
            break;
        case StreamStatus::Connecting:
            state = "Connecting…";
            color = c::kColorAccent;  // жовтий
            break;
        case StreamStatus::Buffering:
            state = "Buffering…";
            color = c::kColorAccent;  // жовтий
            break;
        case StreamStatus::Playing:
            state = "Playing";
            color = c::kColorOk;  // зелений
            break;
        case StreamStatus::Error:
            state = "Error";
            color = c::kColorBad;  // червоний
            break;
        case StreamStatus::Reconnecting:
            state = "Reconnecting…";
            color = c::kColorAccent;  // жовтий
            break;
        default:
            state = "?";
            color = c::kColorDim;
            break;
    }

    D::drawText(state, c::kMargin, c::kStateY, FontSize::Small, color);
}

// ---------------------------------------------------------------------------
// Екрани
// ---------------------------------------------------------------------------
void drawStandby() {
    D::drawIcon(IconId::Standby, c::kStandbyIconX, c::kStandbyIconY, c::kColorDim);
    drawCentered("Standby", c::kStandbyCaptionY, FontSize::Tiny, c::kColorDim);
}

void drawRadio(const AppStateData& s) {
    D::drawText(inputName(s.inputIndex), c::kMargin, c::kTopBarTextY, FontSize::Small, c::kColorDim);
    drawTopIcons(s, true);

    marqueeDraw(s_station, c::kStationY, FontSize::Large, dc::kColorFg);
    marqueeDraw(s_track, c::kTrackY, FontSize::Small, c::kColorDim);

    // TODO (Prompt 13): VU meter
    D::drawRect(c::kVuX, c::kVuY, c::kVuW, c::kVuH, c::kColorDim);

    // [Prompt 10] ДОДАНО: використовуємо StreamStatus замість наївної евристики
    drawStreamStatus(s);

    drawStatusRow(s);
}

void drawExternal(const AppStateData& s) {
    drawTopIcons(s, false);
    drawCentered("INPUT", c::kExtCaptionY, FontSize::Tiny, c::kColorDim);
    drawCentered(inputName(s.inputIndex), c::kExtNameY, FontSize::Large, dc::kColorFg);
    drawStatusRow(s);
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

    AppStateData cmp = s;
    cmp.vuLeft  = 0.0f;   // VU поки не малюється; прибрати в Prompt 13
    cmp.vuRight = 0.0f;
    bool dirty = !s_havePrev || memcmp(&cmp, &s_prev, sizeof(cmp)) != 0;
    if (dirty) {
        s_prev = cmp;
        s_havePrev = true;
    }

    if (s.mode == Mode::Radio) {
        dirty |= marqueeUpdate(s_station, s.stationName[0] ? s.stationName : "No station",
                               FontSize::Large, now);
        dirty |= marqueeUpdate(s_track, s.trackTitle, FontSize::Small, now);
    }

    if (!dirty) return false;   // панель лишається як була

    D::fillScreen(dc::kColorBg);
    switch (s.mode) {
        case Mode::Standby:       drawStandby();                 break;
        case Mode::Radio:         drawRadio(s);                  break;
        case Mode::ExternalInput: drawExternal(s);               break;
        case Mode::Menu:
            if (s.menuContext == MenuContext::StationList) drawStationList(s);
            else                                           drawPlaceholder("Menu (TODO)");
            break;
        case Mode::IrLearn:       drawIrLearn(s);                break;   // [Prompt 15]
        case Mode::WifiSetup:     drawWifiSetup(s);              break;
        default:                  drawPlaceholder("?");          break;
    }
    return true;
}

}  // namespace

bool UiScreens::begin() {
    s_havePrev  = false;
    s_modeKnown = false;
    s_station.seeded = false;
    s_track.seeded   = false;
    DisplayManager::setFrameCallback(&frame);
    return true;
}