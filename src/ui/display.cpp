#include "ui/display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <esp_memory_utils.h>

#include <type_traits>

#include "config/defaults.h"
#include "config/display_config.h"
#include "config/pins.h"
#include "ui/fonts.h"
#include "ui/icons.h"

// ---------------------------------------------------------------------------
// Вибір конфігурації LovyanGFX
// ---------------------------------------------------------------------------
// Використано власний клас-нащадок lgfx::LGFX_Device з lgfx::Panel_ST7789 +
// lgfx::Bus_SPI — офіційний «ручний» спосіб опису пристрою. Автоконфігурації
// (lgfx::LGFX_AutoDetect) не підходять: це самостійна плата з довільними пінами.
// Клас Light_PWM бібліотеки НЕ використовується: підсвіткою керує сам
// DisplayManager через ledcAttach()/ledcWrite() (правило проєкту: лише новий
// LEDC API), а не внутрішній код бібліотеки з невідомим набором викликів LEDC.
//
// Буфер: 2 повнокадрові спрайти (double buffer, MASTER SPEC розділ 5) —
// 2 × 320·170·2 ≈ 217 КБ, це ≈ 2.6 % від 8 МБ PSRAM. Відсутність мерехтіння
// забезпечує сам принцип «кадр повністю малюється в спрайт поза екраном, потім
// одним потоком іде на панель» — вона не залежить від кількості буферів.
// Другий буфер потрібен для перекриття: із display_cfg::kUseDma = true кадр A
// передається DMA у фоні, поки задача малює кадр B; без DMA виграшу від
// другого буфера немає (передача блокуюча), він лише готує архітектуру.

namespace {

constexpr const char* kTag = "[DISP]";

// ---------------------------------------------------------------------------
// Пристрій LovyanGFX
// ---------------------------------------------------------------------------
class PanelDevice : public lgfx::LGFX_Device {
public:
    PanelDevice() {
        {
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;                // FSPI
            cfg.spi_mode   = display_cfg::kSpiMode;
            cfg.freq_write = display_cfg::kSpiWriteHz;
            cfg.spi_3wire  = false;                    // окремий MISO не потрібен (пін -1)
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk   = pins::kSt7789Sclk;
            cfg.pin_mosi   = pins::kSt7789Mosi;
            cfg.pin_miso   = -1;
            cfg.pin_dc     = pins::kSt7789Dc;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs   = pins::kSt7789Cs;
            cfg.pin_rst  = pins::kSt7789Rst;           // апаратний скид у init()
            cfg.pin_busy = -1;

            cfg.panel_width    = display_cfg::kPanelWidth;
            cfg.panel_height   = display_cfg::kPanelHeight;
            cfg.memory_width   = display_cfg::kMemoryWidth;
            cfg.memory_height  = display_cfg::kMemoryHeight;
            // ПРИПУЩЕННЯ: див. коментар у display_config.h.
            cfg.offset_x       = display_cfg::kOffsetX;
            cfg.offset_y       = display_cfg::kOffsetY;
            cfg.offset_rotation = 0;

            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable   = false;                    // MISO не підключений
            cfg.invert     = display_cfg::kInvertColors;
            cfg.rgb_order  = display_cfg::kBgrOrder;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }

private:
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
};

PanelDevice s_lcd;
lgfx::LGFX_Sprite s_spr0(&s_lcd);
lgfx::LGFX_Sprite s_spr1(&s_lcd);
lgfx::LGFX_Sprite* const s_bufs[display_cfg::kBufferCount] = {&s_spr0, &s_spr1};

static_assert(display_cfg::kBufferCount == 2, "double buffering expects exactly 2 buffers");

// ---------------------------------------------------------------------------
// Стан
// ---------------------------------------------------------------------------
volatile uint8_t s_back = 0;                 // індекс буфера, у який малюємо (лише display-задача)
bool s_ready = false;                        // ресурси створено, задача запущена
bool s_blAttached = false;                   // ledcAttach() виконано
volatile bool s_blArmed = false;             // перший кадр виведено — підсвітку можна вмикати
volatile uint8_t s_brightness = defaults::kDefaultBrightness;
volatile bool s_flipRequested = display_cfg::kDefaultFlipped;  // бажана орієнтація
bool s_flipApplied = display_cfg::kDefaultFlipped;             // застосована (лише display-задача)
DisplayManager::FrameCallback volatile s_frameCb = nullptr;
DisplayError s_error = DisplayError::NotStarted;

// ---------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------

// init() у різних версіях LovyanGFX повертає bool або void — приймаємо обидва.
template <typename T>
bool initDevice(T& dev) {
    if constexpr (std::is_void_v<decltype(dev.init())>) {
        dev.init();
        return true;
    } else {
        return static_cast<bool>(dev.init());
    }
}

uint32_t backlightDuty(uint8_t percent) {
    constexpr uint32_t kMaxDuty = (1u << display_cfg::kBacklightResolutionBits) - 1u;
    const uint32_t duty = (kMaxDuty * percent) / 100u;
    return display_cfg::kBacklightActiveHigh ? duty : (kMaxDuty - duty);
}

void applyBacklight() {
    if (!s_blAttached) {
        return;
    }
    const uint8_t percent = s_blArmed ? s_brightness : 0;
    ledcWrite(pins::kSt7789Blk, backlightDuty(percent));
}

uint8_t rotationFor(bool flipped) {
    return flipped ? display_cfg::kRotationFlipped : display_cfg::kRotationNormal;
}

// Викликається лише з display-задачі (і з begin() до старту задачі).
// Повертає true, якщо поворот панелі змінився.
bool applyRotation() {
    const bool want = s_flipRequested;
    if (want == s_flipApplied) {
        return false;
    }
    if (display_cfg::kUseDma) {
        s_lcd.waitDMA();
    }
    s_lcd.setRotation(rotationFor(want));
    s_flipApplied = want;
    Serial.printf("%s rotation %u (%s)\n", kTag, static_cast<unsigned>(rotationFor(want)),
                  want ? "flipped 180" : "normal");
    return true;
}

void releaseSprites() {
    for (auto* spr : s_bufs) {
        spr->deleteSprite();
    }
}

lgfx::LGFX_Sprite& backBuffer() {
    return *s_bufs[s_back & 1u];
}

void pushBuffer(uint8_t idx) {
    lgfx::LGFX_Sprite* spr = s_bufs[idx];
    if (display_cfg::kUseDma) {
        // Спрайт RGB565 зберігає байти вже в порядку шини, тому тип swap565_t.
        s_lcd.startWrite();
        s_lcd.pushImageDMA(0, 0, display_cfg::kWidth, display_cfg::kHeight,
                           reinterpret_cast<const lgfx::swap565_t*>(spr->getBuffer()));
        s_lcd.endWrite();
    } else {
        spr->pushSprite(&s_lcd, 0, 0);
    }
}

void taskEntry(void*) {
    DisplayManager::taskLoop();
    vTaskDelete(nullptr);  // taskLoop не повертається; захист від випадкового виходу
}

#if DISPLAY_DEMO
// Статичний тестовий кадр. Малює лише через публічні примітиви DisplayManager.
void drawDemoFrame() {
    using namespace display_cfg;
    namespace d = display_cfg::demo;

    DisplayManager::fillScreen(kColorBg);

    // Рамка по краю: при неправильному зміщенні вікна якась сторона зникне,
    // а поруч зʼявиться смуга «шуму» з неініціалізованої памʼяті панелі.
    for (int16_t i = 0; i < d::kFrameThickness; ++i) {
        DisplayManager::drawRect(i, i, kWidth - 2 * i, kHeight - 2 * i, kColorWhite);
    }

    // Кольорові кути: лівий верхній червоний, правий верхній зелений,
    // лівий нижній синій, правий нижній жовтий (перевірка орієнтації й RGB/BGR).
    const int16_t lo = d::kCornerInset;
    const int16_t hiX = kWidth - d::kCornerInset - d::kCornerSize;
    const int16_t hiY = kHeight - d::kCornerInset - d::kCornerSize;
    DisplayManager::fillRect(lo,  lo,  d::kCornerSize, d::kCornerSize, kColorRed);
    DisplayManager::fillRect(hiX, lo,  d::kCornerSize, d::kCornerSize, kColorGreen);
    DisplayManager::fillRect(lo,  hiY, d::kCornerSize, d::kCornerSize, kColorBlue);
    DisplayManager::fillRect(hiX, hiY, d::kCornerSize, d::kCornerSize, kColorYellow);

    // Проблемні літери великим шрифтом і меншим.
    DisplayManager::drawText("Ґрунт Їжачок Єдність", d::kTextX, d::kTitleY,
                             FontSize::Large, kColorYellow);
    DisplayManager::drawText("Ґанок, їжак, єнот, ялинка, щастя", d::kTextX, d::kSubtitleY,
                             FontSize::Small, kColorWhite);

    // Повний алфавіт (33 + 33 літери) розміром Tiny: відсутні гліфи видно одразу.
    DisplayManager::drawText("АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ", d::kTextX, d::kAlphaUpperY,
                             FontSize::Tiny, kColorGray);
    DisplayManager::drawText("абвгґдеєжзиіїйклмнопрстуфхцчшщьюя", d::kTextX, d::kAlphaLowerY,
                             FontSize::Tiny, kColorGray);

    // Усі іконки в ряд із підписами по центру комірки.
    for (uint8_t i = 0; i < UiIcons::kCount; ++i) {
        const IconId id = UiIcons::fromIndex(i);
        const int16_t slotX = d::kTextX + static_cast<int16_t>(i) * d::kIconSlotW;
        DisplayManager::drawIcon(id, slotX + (d::kIconSlotW - kIconSize) / 2, d::kIconRowY,
                                 kColorWhite);

        const char* label = UiIcons::name(id);
        const int16_t w = static_cast<int16_t>(UiFonts::textWidth(label, FontSize::Tiny));
        DisplayManager::drawText(label, slotX + (d::kIconSlotW - w) / 2, d::kCaptionY,
                                 FontSize::Tiny, kColorGray);
    }

    // Параметри панелі, з якими зібрано прошивку.
    char info[64];
    snprintf(info, sizeof(info), "off=%d,%d rot=%u spi=%luMHz dma=%d inv=%d bgr=%d",
             static_cast<int>(kOffsetX), static_cast<int>(kOffsetY),
             static_cast<unsigned>(rotationFor(s_flipApplied)),
             static_cast<unsigned long>(kSpiWriteHz / 1000000UL),
             kUseDma ? 1 : 0, kInvertColors ? 1 : 0, kBgrOrder ? 1 : 0);
    DisplayManager::drawText(info, d::kTextX, d::kInfoY, FontSize::Tiny, kColorGray);
}
#endif  // DISPLAY_DEMO

}  // namespace

// ---------------------------------------------------------------------------
// Керування
// ---------------------------------------------------------------------------

bool DisplayManager::begin() {
    if (s_ready) {
        return true;
    }

    // 1. PSRAM (буфери кадру за правилом проєкту лише в PSRAM).
    if (!psramFound()) {
        s_error = DisplayError::NoPsram;
        Serial.printf("%s PSRAM not found, display disabled\n", kTag);
        return false;
    }

    // 2. Шрифти й іконки.
    if (!UiFonts::begin() || !UiIcons::begin()) {
        s_error = DisplayError::ResourceInitFailed;
        Serial.printf("%s fonts/icons init failed\n", kTag);
        return false;
    }

    // 3. Підсвітка: LEDC, спочатку вимкнена (до першого кадру екран чорний).
    if (!s_blAttached) {
        if (!ledcAttach(pins::kSt7789Blk, display_cfg::kBacklightFreqHz,
                        display_cfg::kBacklightResolutionBits)) {
            s_error = DisplayError::BacklightInitFailed;
            Serial.printf("%s backlight ledcAttach failed\n", kTag);
            return false;
        }
        s_blAttached = true;
    }
    s_blArmed = false;
    applyBacklight();

    // 4. Панель (апаратний скид RST усередині init()).
    if (!initDevice(s_lcd)) {
        s_error = DisplayError::PanelInitFailed;
        Serial.printf("%s panel init failed\n", kTag);
        return false;
    }
    s_flipApplied = s_flipRequested;
    s_lcd.setRotation(rotationFor(s_flipApplied));
    if (s_lcd.width() != display_cfg::kWidth || s_lcd.height() != display_cfg::kHeight) {
        s_error = DisplayError::PanelInitFailed;
        Serial.printf("%s panel size %dx%d, expected %dx%d (check kRotationNormal)\n", kTag,
                      static_cast<int>(s_lcd.width()), static_cast<int>(s_lcd.height()),
                      static_cast<int>(display_cfg::kWidth),
                      static_cast<int>(display_cfg::kHeight));
        return false;
    }
    s_lcd.fillScreen(display_cfg::kColorBlack);

    // 5. Буфери кадру в PSRAM.
    const size_t psramBefore = ESP.getFreePsram();
    for (auto* spr : s_bufs) {
        spr->setPsram(true);
        spr->setColorDepth(16);
        if (!spr->createSprite(display_cfg::kWidth, display_cfg::kHeight) ||
            spr->getBuffer() == nullptr || !esp_ptr_external_ram(spr->getBuffer())) {
            s_error = DisplayError::SpriteAllocFailed;
            Serial.printf("%s frame buffer alloc in PSRAM failed\n", kTag);
            releaseSprites();
            return false;
        }
    }
    Serial.printf("%s buffers: %u x %ux%u RGB565 in PSRAM (%u bytes used)\n", kTag,
                  static_cast<unsigned>(display_cfg::kBufferCount),
                  static_cast<unsigned>(display_cfg::kWidth),
                  static_cast<unsigned>(display_cfg::kHeight),
                  static_cast<unsigned>(psramBefore - ESP.getFreePsram()));

    // 6. Задача малювання.
    s_back = 0;
    s_ready = true;  // до створення задачі: вона одразу починає малювати
    const BaseType_t ok = xTaskCreatePinnedToCore(
        taskEntry, "display", display_cfg::kTaskStackBytes, nullptr,
        display_cfg::kTaskPriority, nullptr, display_cfg::kTaskCore);
    if (ok != pdPASS) {
        s_ready = false;
        s_error = DisplayError::TaskCreateFailed;
        Serial.printf("%s task create failed\n", kTag);
        releaseSprites();
        return false;
    }

    s_error = DisplayError::None;
    Serial.printf("%s ready: %dx%d, spi=%luMHz, frame=%lums, core=%d prio=%u\n", kTag,
                  static_cast<int>(display_cfg::kWidth), static_cast<int>(display_cfg::kHeight),
                  static_cast<unsigned long>(display_cfg::kSpiWriteHz / 1000000UL),
                  static_cast<unsigned long>(display_cfg::kFramePeriodMs),
                  display_cfg::kTaskCore, static_cast<unsigned>(display_cfg::kTaskPriority));
    return true;
}

void DisplayManager::taskLoop() {
#if DISPLAY_DEMO
    drawDemoFrame();
#else
    fillScreen(display_cfg::kColorBg);
#endif
    const uint32_t t0 = millis();
    present();
    Serial.printf("%s first frame pushed in %lu ms, backlight %u%%\n", kTag,
                  static_cast<unsigned long>(millis() - t0),
                  static_cast<unsigned>(s_brightness));

    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        // Зміна орієнтації: поворот панелі + повторний вивід останнього кадру
        // (він у «передньому» буфері s_back^1, після present() не міняється).
        const bool rotated = applyRotation();

        FrameCallback cb = s_frameCb;
        if (cb != nullptr && cb()) {
            present();
        } else if (rotated) {
            pushBuffer((s_back ^ 1u) & 1u);
        }
        // [Prompt 35] xTaskDelayUntil НЕ блокує, якщо кадр тривав довше за період (повертає pdFALSE):
        // за серії таких кадрів задача на ядрі 0 монополізувала б CPU й голодила IDLE0 -> task_wdt.
        // Тоді примусово віддаємо процесор мінімум на один тік і перезапускаємо відлік.
        if (xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(display_cfg::kFramePeriodMs)) == pdFALSE) {
            vTaskDelay(1);
            lastWake = xTaskGetTickCount();
        }
    }
}

DisplayError DisplayManager::lastError() {
    return s_error;
}

const char* DisplayManager::lastErrorName() {
    switch (s_error) {
        case DisplayError::None:                return "ok";
        case DisplayError::NotStarted:          return "not started";
        case DisplayError::NoPsram:             return "PSRAM not found";
        case DisplayError::ResourceInitFailed:  return "fonts/icons init failed";
        case DisplayError::BacklightInitFailed: return "backlight init failed";
        case DisplayError::PanelInitFailed:     return "panel init failed";
        case DisplayError::SpriteAllocFailed:   return "frame buffer alloc failed";
        case DisplayError::TaskCreateFailed:    return "task create failed";
    }
    return "?";
}

bool DisplayManager::isReady() {
    return s_ready;
}

void DisplayManager::setBrightness(uint8_t percent) {
    s_brightness = (percent > 100) ? 100 : percent;
    applyBacklight();
}

uint8_t DisplayManager::brightness() {
    return s_brightness;
}

void DisplayManager::setFrameCallback(FrameCallback callback) {
    s_frameCb = callback;
}

void DisplayManager::setFlipped(bool flipped) {
    s_flipRequested = flipped;
}

bool DisplayManager::isFlipped() {
    return s_flipRequested;
}

int16_t DisplayManager::width() {
    return display_cfg::kWidth;
}

int16_t DisplayManager::height() {
    return display_cfg::kHeight;
}

// ---------------------------------------------------------------------------
// Примітиви (поверх заднього буфера)
// ---------------------------------------------------------------------------

void DisplayManager::fillScreen(uint16_t color) {
    if (!s_ready) return;
    backBuffer().fillScreen(color);
}

void DisplayManager::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (!s_ready) return;
    backBuffer().fillRect(x, y, w, h, color);
}

void DisplayManager::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (!s_ready) return;
    backBuffer().drawRect(x, y, w, h, color);
}

void DisplayManager::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    if (!s_ready) return;
    backBuffer().drawLine(x0, y0, x1, y1, color);
}

void DisplayManager::drawText(const char* utf8, int16_t x, int16_t y, FontSize size,
                              uint16_t fg) {
    if (!s_ready || utf8 == nullptr) return;
    lgfx::LGFX_Sprite& spr = backBuffer();
    spr.setFont(UiFonts::font(size));
    spr.setTextColor(fg);  // один колір = прозоре тло
    spr.drawString(utf8, x, y);
}

void DisplayManager::drawText(const char* utf8, int16_t x, int16_t y, FontSize size,
                              uint16_t fg, uint16_t bg) {
    if (!s_ready || utf8 == nullptr) return;
    lgfx::LGFX_Sprite& spr = backBuffer();
    spr.setFont(UiFonts::font(size));
    spr.setTextColor(fg, bg);
    spr.drawString(utf8, x, y);
}

void DisplayManager::drawIcon(IconId id, int16_t x, int16_t y, uint16_t color) {
    if (!s_ready) return;
    const uint8_t* mask = UiIcons::mask(id);
    if (mask == nullptr) return;
    backBuffer().drawBitmap(x, y, mask, display_cfg::kIconSize, display_cfg::kIconSize, color);
}

void DisplayManager::present() {
    if (!s_ready) return;

    // Попередня DMA-передача (якщо була) мусить завершитись до нової.
    if (display_cfg::kUseDma) {
        s_lcd.waitDMA();
    }

    applyRotation();  // якщо запит на поворот прийшов між кадрами

    const uint8_t idx = s_back & 1u;
    pushBuffer(idx);
    s_back = idx ^ 1u;  // наступний кадр малюємо в інший буфер

    // Підсвітка вмикається лише після виводу першого кадру (без спалаху сміття).
    if (!s_blArmed) {
        if (display_cfg::kUseDma) {
            s_lcd.waitDMA();
        }
        s_blArmed = true;
        applyBacklight();
    }
}