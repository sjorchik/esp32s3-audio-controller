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
