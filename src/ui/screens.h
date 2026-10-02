#pragma once

// Екрани UI (Prompt 9).
// Реєструє FrameCallback у DisplayManager. Колбек викликається з display-задачі
// і малює примітивами DisplayManager.
//
// Джерела даних (ізоляція):
//  - AppState::snapshot() — увесь динамічний стан пристрою;
//  - UiFonts / UiIcons — ресурси відображення;
//  - [Prompt 11] StationStore::count()/get() — список станцій для екрана
//    Mode::Menu/StationList. StationStore — модуль чистих даних (не залізо, не
//    читає EventBus, не впливає на звуковий тракт), тому читати його напряму
//    дозволено; правило «AppController — єдиний власник заліза» не порушується.
// UiScreens не володіє задачею, не читає EventBus, не знає про спрайти.

class UiScreens {
public:
    // Реєструє FrameCallback. Викликати ПІСЛЯ DisplayManager::begin().
    // Повертає true завжди (реєстрація вказівника не може не вдатися).
    static bool begin();
};
