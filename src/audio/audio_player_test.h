#pragma once

// Тестовий режим аудіоплеєра: Serial-команди для ручної перевірки без
// AppController, станцій і Wi-Fi-менеджера.
// Компілюється лише при AUDIO_PLAYER_TEST == 1 (config/audio_player_config.h).
// Протокол команд — у audio_player_test.cpp (команда `help` друкує його в Serial).
//
// УВАГА: читає той самий Serial, що й AudioProcTest, тому main.cpp не запускає
// їх одночасно (при AUDIO_PLAYER_TEST == 1 AudioProcTest пропускається).

#include "config/audio_player_config.h"

#if AUDIO_PLAYER_TEST

class AudioProcessor;

class AudioPlayerTest {
public:
    // Запускає задачу читання Serial (ядро 0, пріоритет 1). Повторний виклик
    // безпечний. processorOrNull — лише для команд `vol` і `pmute` (може бути
    // nullptr); плеєр сам процесор не чіпає.
    static bool begin(AudioProcessor* processorOrNull);
};

#endif  // AUDIO_PLAYER_TEST
