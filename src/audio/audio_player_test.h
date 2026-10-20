#pragma once

// Тестовий режим аудіоплеєра: Serial-команди для ручної перевірки без
// AppController, станцій і Wi-Fi-менеджера.
// Компілюється лише при AUDIO_PLAYER_TEST == 1 (config/audio_player_config.h).
// Протокол команд — у audio_player_test.cpp (команда `help` друкує його в Serial).
//
// [Prompt 48a] Аудіопроцесор вирізано: команди `vol` / `pmute` прибрано, begin() без параметрів.

#include "config/audio_player_config.h"

#if AUDIO_PLAYER_TEST

class AudioPlayerTest {
public:
    // Запускає задачу читання Serial (ядро 0, пріоритет 1). Повторний виклик
    // безпечний.
    static bool begin();
};

#endif  // AUDIO_PLAYER_TEST
