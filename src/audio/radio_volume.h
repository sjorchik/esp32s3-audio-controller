#pragma once

// Цифрова гучність РАДІО (Prompt 47a): один екземпляр DspVolume, який обробляє декодований PCM у хуку
// audio_process_i2s (audio/vu_pcm_hook.cpp, ядро 1) ОСТАННІМ кроком ланцюга:
//   EQ -> вимір VU -> output_trim (рівень станції) -> radio_volume (гучність користувача) -> I2S.
//
// Потоки: setAttenuationDb()/setMute()/requestForceMute()/isSilent()/muted() — з будь-якої задачі
// (AppController, ядро 0); process() — ЛИШЕ з хука. Екземпляр стартує в МЮТІ (повна шкала
// на вихід неможлива до першого setMute(false)).
//
// requestForceMute(): миттєва тиша, яку виконає ПЕРШИЙ наступний блок хука (потрібна, коли звуку
// вже немає або згасання не встигло). Якщо блоки не йдуть, нічого не звучить і так.

#include <stdint.h>

namespace radio_volume {

// Послаблення, дБ (обрізається до dsp_volume_cfg::kMinDb..kMaxDb). Перехід — у наступному блоці.
void setAttenuationDb(float db);
float attenuationDb();

// Мʼют із плавним переходом (kMuteRampMs вниз, kRampMs вгору).
void setMute(bool mute);
bool muted();

// Миттєвий мʼют у наступному блоці хука (будь-яка задача).
void requestForceMute();

// Мʼют увімкнено і згасання завершено.
bool isSilent();

// Лише з хука: множення інтерліврованого стерео-блоку int32 на місці.
void process(int32_t* buf, int32_t frames);

}  // namespace radio_volume