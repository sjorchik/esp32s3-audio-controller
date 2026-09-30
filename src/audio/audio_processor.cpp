#include "audio/audio_processor.h"

#include <new>

#include "config/features.h"

#if ENABLE_TDA7318
#include "audio/tda7318.h"
#endif
#if ENABLE_PT2313L
#include "audio/pt2313l.h"
#endif

AudioProcessor* createAudioProcessor(AudioProcType type) {
    // Звичайний new (внутрішня RAM, не PSRAM): обʼєкт малий і живе весь час роботи.
    // nothrow: винятки в проєкті вимкнені, при браку памʼяті повертаємо nullptr.
    switch (type) {
#if ENABLE_TDA7318
        case AudioProcType::Tda7318:
            return new (std::nothrow) Tda7318();
#endif
#if ENABLE_PT2313L
        case AudioProcType::Pt2313l:
            return new (std::nothrow) Pt2313l();
#endif
        default:
            break;
    }
    return nullptr;
}
