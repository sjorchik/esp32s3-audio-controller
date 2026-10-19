// Перехоплення PCM (гарячий шлях аудіо, ядро 1): VU-метр (Prompt 17), рівень виходу
// декодера по станціях (Prompt 25), еквалайзер радіо (Prompt 30) і ЦИФРОВА ГУЧНІСТЬ радіо (Prompt 47a).
//
// ВАЖЛИВО: цей файл НЕ включає Audio.h. Там audio_process_i2s оголошено як
// extern __attribute__((weak)); визначення, що йде після такого оголошення, теж стає
// слабким, і лінкер залишав порожню 5-байтову заглушку з Audio.cpp (хук не викликався).
// Без цього оголошення наше визначення сильне й перекриває слабку заглушку.
//
// Сигнатуру звірено з Audio.h/Audio.cpp збірки (символ _Z17audio_process_i2sPlsPb):
//   void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S)
// Викликається з Audio::playChunk() ПІСЛЯ гучності/gain бібліотеки і ПЕРЕД i2s_channel_write().
//   - outBuff — інтерліврований стерео L,R,L,R...; кожне слово int32 = один відлік
//     каналу, 16-бітні дані вирівняні вліво (повна шкала 2^31);
//   - validSamples — кількість СЛІВ int32 (обох каналів разом): кадрів = /2;
//   - *continueI2S = true ОБОВʼЯЗКОВО: false = бібліотека пропускає запис блоку -> тиша.
//
// Порядок обробки блоку (P47a):
//   eq::process (EQ радіо, на місці) -> вимір VU -> output_trim::process (рівень станції)
//   -> radio_volume::process (гучність користувача, мʼют). VU міряється ДО обох послаблень (P34/P35).
// Без логування, блокувань і millis(); мінімум обчислень.
//
// [Prompt 47a] Без хука гучності радіо НЕ керується (повна шкала ЦАП на підсилювач), тому
// VU_PCM_HOOK_STYLE != 1 -> #error (раніше #warning). Компілюється незалежно від ENABLE_VU
// (публікація піків — лише при ENABLE_VU).

#include <stdint.h>

#include "audio/eq.h"
#include "audio/output_trim.h"
#include "audio/radio_volume.h"
#include "audio/vu_source.h"
#include "config/features.h"
#include "config/vu_config.h"

#if VU_PCM_HOOK_STYLE != 1
#error "VU_PCM_HOOK_STYLE must be 1: audio_process_i2s hook carries radio digital volume (P47a), station level (P25) and EQ (P30)"
#endif

void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
    if (continueI2S != nullptr) {
        *continueI2S = true;
    }
    if (outBuff == nullptr || validSamples < 2) {
        return;
    }
    int32_t frames = static_cast<int32_t>(validSamples) / 2;
    // [Prompt 30] Еквалайзер радіо (усі смуги 0 дБ — блок не чіпається).
    eq::process(outBuff, frames);
#if ENABLE_VU
    // [Prompt 35] VU міряється ПІСЛЯ еквалайзера, але ДО рівня станції і гучності користувача:
    // 0 дБ індикатора = повна шкала сигналу декодера.
    int32_t scan = frames;
    if (scan > vu_cfg::kMaxFramesPerBlock) {
        scan = vu_cfg::kMaxFramesPerBlock;  // захист від виходу за буфер
    }
    uint32_t peakL = 0;
    uint32_t peakR = 0;
    const int32_t* p = outBuff;
    for (int32_t i = 0; i < scan; ++i) {
        const int32_t l = p[0];
        const int32_t r = p[1];
        p += 2;
        // Модуль без UB для INT32_MIN: 0u - (uint32_t)l = 2^31.
        const uint32_t al = (l < 0) ? (0u - static_cast<uint32_t>(l)) : static_cast<uint32_t>(l);
        const uint32_t ar = (r < 0) ? (0u - static_cast<uint32_t>(r)) : static_cast<uint32_t>(r);
        if (al > peakL) peakL = al;
        if (ar > peakR) peakR = ar;
    }
#endif  // ENABLE_VU
    // [Prompt 25] Рівень станції: послаблюємо ВЕСЬ блок (validSamples — довжина буфера).
    output_trim::process(outBuff, frames);
    // [Prompt 47a] Гучність користувача (дБ) і мʼют: останній крок; стартує замʼюченою.
    radio_volume::process(outBuff, frames);
#if ENABLE_VU
    // 2^31 -> 2^15: шкала vu_cfg::kFullScale (32768).
    VuSourceDecodedPcm::publishPeaks(peakL >> 16, peakR >> 16, static_cast<uint32_t>(scan));
#endif  // ENABLE_VU
}