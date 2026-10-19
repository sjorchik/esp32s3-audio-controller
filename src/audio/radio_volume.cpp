// Цифрова гучність радіо (Prompt 47a). Опис — у radio_volume.h.
//
// ВАЖЛИВО: файл НЕ включає Audio.h (як і vu_pcm_hook.cpp): лише чиста математика через DspVolume.

#include "audio/radio_volume.h"

#include "audio/dsp_volume.h"

namespace {

// Початковий стан DspVolume: мʼют, підсилення 0 -> до першого setMute(false) блоки множаться на 0.
DspVolume s_vol;

}  // namespace

void radio_volume::setAttenuationDb(float db) {
    s_vol.setAttenuationDb(db);
}

float radio_volume::attenuationDb() {
    return s_vol.attenuationDb();
}

void radio_volume::setMute(bool mute) {
    s_vol.setMute(mute);
}

bool radio_volume::muted() {
    return s_vol.muted();
}

void radio_volume::requestForceMute() {
    s_vol.requestForceMute();
}

bool radio_volume::isSilent() {
    return s_vol.isSilent();
}

void radio_volume::process(int32_t* buf, int32_t frames) {
    s_vol.process(buf, frames);
}