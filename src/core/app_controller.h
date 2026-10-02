#pragma once

// Контролер логіки режимів і розкладки керування.
// Читає EventBus у власній задачі (ядро 0, пріоритет 3) і керує AudioProcessor,
// AudioPlayer, Settings та AppState за розкладкою з розділу 5 MASTER SPEC.
// Сам нічого не малює й не чіпає DisplayManager: екрани читають AppState.
//
// [Prompt 8] Заглушку замінено.
//   ЗМІНЕНО — begin() тепер приймає вказівник на аудіопроцесор (AppController
//             ним не володіє; main.cpp створив його раніше). nullptr дозволений:
//             тоді керування звуком недоступне, решта логіки працює.

#include "core/events.h"

class AudioProcessor;

class AppController {
public:
    // Ініціалізація: AppState::begin() (першим ділом), стан із Settings,
    // створення задачі. Викликати ПІСЛЯ EventBus::begin(), SettingsStore::begin()
    // /load(), створення аудіопроцесора та AudioPlayer::begin().
    // Повторний виклик безпечний (повертає true).
    static bool begin(AudioProcessor* processorOrNull);

    // Обробка однієї події. Основний шлях — внутрішня задача, яка сама читає
    // чергу; публічний виклик лишено для тестування. Захищено внутрішнім
    // мʼютексом, тож безпечний і з інших задач. Не робити з обробника подій.
    static void handleEvent(const Event& event);
};
