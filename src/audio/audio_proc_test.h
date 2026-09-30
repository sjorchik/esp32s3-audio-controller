#pragma once

// Тестовий режим аудіопроцесора: Serial-команди для ручної перевірки без решти проекту.
// Компілюється лише при AUDIO_PROC_TEST == 1 (config/audio_config.h).
// Протокол команд — у audio_proc_test.cpp (команда `h` друкує його в Serial).

#include "config/audio_config.h"

#if AUDIO_PROC_TEST

class AudioProcessor;

class AudioProcTest {
public:
    // Запускає задачу читання Serial (ядро 0, пріоритет 1). Повторний виклик безпечний.
    // proc має жити весь час роботи (його створює createAudioProcessor).
    static bool begin(AudioProcessor* proc);
};

#endif  // AUDIO_PROC_TEST
