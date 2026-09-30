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
