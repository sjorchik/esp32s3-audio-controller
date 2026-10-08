// Перехоплення PCM (гарячий шлях аудіо, ядро 1): VU-метр (Prompt 17) і рівень виходу
// декодера по станціях (Prompt 25).
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
// [Prompt 30] Перед рівнем станції блок проходить 5-смуговий еквалайзер (eq::process, на місці);
// порядок: EQ -> VU (вимір) -> output_trim [P35: VU більше не після послаблення станції].
// [Prompt 25] Буфер тепер і ЗМІНЮЄТЬСЯ: output_trim::process() послаблює блок на місці
// (рівень поточної станції), а VU міряє вже послаблений сигнал (реальний вихід). Без логування,
// блокувань і millis(); мінімум обчислень.
//
// Хук потрібен і для рівня станцій, тож компілюється при VU_PCM_HOOK_STYLE == 1 незалежно від
// ENABLE_VU (публікація піків — лише при ENABLE_VU). При VU_PCM_HOOK_STYLE == 0 рівень станцій
// НЕ діє (див. #warning нижче).

#include <stdint.h>

#include "audio/eq.h"
#include "audio/output_trim.h"
#include "audio/vu_source.h"
#include "config/features.h"
#include "config/vu_config.h"

#if VU_PCM_HOOK_STYLE == 1
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
    // [Prompt 35] VU міряється ПІСЛЯ еквалайзера, але ДО рівня станції: послаблення станції
    // (типово -20 дБ) не стискає шкалу, і 0 дБ індикатора = повна шкала сигналу декодера.
    // Раніше (P25) VU йшов після output_trim.
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
#if ENABLE_VU
    // 2^31 -> 2^15: шкала vu_cfg::kFullScale (32768).
    VuSourceDecodedPcm::publishPeaks(peakL >> 16, peakR >> 16, static_cast<uint32_t>(scan));
#endif  // ENABLE_VU
}
#else
#warning "VU_PCM_HOOK_STYLE != 1: audio_process_i2s hook is absent, per-station output level (Prompt 25) and radio EQ (Prompt 30) will NOT be applied"
#endif