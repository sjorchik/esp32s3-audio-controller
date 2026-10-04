// Перехоплення PCM для VU (Prompt 17), гарячий шлях аудіо (ядро 1).
//
// ВАЖЛИВО: цей файл НЕ включає Audio.h. Там audio_process_i2s оголошено як
// extern __attribute__((weak)); визначення, що йде після такого оголошення, теж стає
// слабким, і лінкер залишав порожню 5-байтову заглушку з Audio.cpp (хук не викликався).
// Без цього оголошення наше визначення сильне й перекриває слабку заглушку.
//
// Сигнатуру звірено з Audio.h/Audio.cpp збірки (символ _Z17audio_process_i2sPlsPb):
//   void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S)
// Викликається з Audio::playChunk() ПІСЛЯ гучності/gain і ПЕРЕД i2s_channel_write().
//   - outBuff — інтерліврований стерео L,R,L,R...; кожне слово int32 = один відлік
//     каналу, 16-бітні дані вирівняні вліво (повна шкала 2^31);
//   - validSamples — кількість СЛІВ int32 (обох каналів разом): кадрів = /2;
//   - *continueI2S = true ОБОВʼЯЗКОВО: false = бібліотека пропускає запис блоку -> тиша.
// Буфер лише читаємо. Без логування, блокувань і millis(); мінімум обчислень (пік |семпла|).

#include <stdint.h>

#include "audio/vu_source.h"
#include "config/features.h"
#include "config/vu_config.h"

#if ENABLE_VU && VU_PCM_HOOK_STYLE == 1
void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
    if (continueI2S != nullptr) {
        *continueI2S = true;
    }
    if (outBuff == nullptr || validSamples < 2) {
        return;
    }
    int32_t frames = static_cast<int32_t>(validSamples) / 2;
    if (frames > vu_cfg::kMaxFramesPerBlock) {
        frames = vu_cfg::kMaxFramesPerBlock;  // захист від виходу за буфер
    }
    uint32_t peakL = 0;
    uint32_t peakR = 0;
    const int32_t* p = outBuff;
    for (int32_t i = 0; i < frames; ++i) {
        const int32_t l = p[0];
        const int32_t r = p[1];
        p += 2;
        // Модуль без UB для INT32_MIN: 0u - (uint32_t)l = 2^31.
        const uint32_t al = (l < 0) ? (0u - static_cast<uint32_t>(l)) : static_cast<uint32_t>(l);
        const uint32_t ar = (r < 0) ? (0u - static_cast<uint32_t>(r)) : static_cast<uint32_t>(r);
        if (al > peakL) peakL = al;
        if (ar > peakR) peakR = ar;
    }
    // 2^31 -> 2^15: шкала vu_cfg::kFullScale (32768).
    VuSourceDecodedPcm::publishPeaks(peakL >> 16, peakR >> 16, static_cast<uint32_t>(frames));
}
#endif
