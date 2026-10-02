#pragma once

// Екрани UI (Prompt 9).
// Реєструє FrameCallback у DisplayManager. Колбек викликається з display-задачі,
// читає лише AppState::snapshot() і малює примітивами DisplayManager.
// UiScreens не володіє задачею, не читає EventBus, не знає про спрайти.

class UiScreens {
public:
    // Реєструє FrameCallback. Викликати ПІСЛЯ DisplayManager::begin().
    // Повертає true завжди (реєстрація вказівника не може не вдатися).
    static bool begin();
};