// Цифрова гучність і мʼют (Prompt 45, доповнено Prompt 47a). Опис і правила потоків — у dsp_volume.h.
//
// ВАЖЛИВО: файл НЕ включає Audio.h. Чиста математика.

#include "audio/dsp_volume.h"

#include <math.h>

namespace {

constexpr float kOutMax = 2147483520.0f;
constexpr float kOutMin = -2147483648.0f;

inline int32_t toInt32(float y) {
    if (y > kOutMax) y = kOutMax;
    if (y < kOutMin) y = kOutMin;
    return static_cast<int32_t>(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

// Довжина переходу в кадрах (мінімум 1).
uint32_t framesFor(uint32_t ms, uint32_t hz) {
    const uint32_t f = hz * ms / 1000u;
    return f > 0 ? f : 1;
}

float dbToGain(float db) {
    return powf(10.0f, db / 20.0f);
}

}  // namespace

void DspVolume::setSampleRate(uint32_t hz) {
    if (hz < dsp_volume_cfg::kMinSampleRateHz || hz > dsp_volume_cfg::kMaxSampleRateHz) {
        return;
    }
    hz_ = hz;
}

void DspVolume::setAttenuationDb(float db) {
    if (!(db >= dsp_volume_cfg::kMinDb)) {  // також обробляє NaN
        db = dsp_volume_cfg::kMinDb;
    }
    if (db > dsp_volume_cfg::kMaxDb) {
        db = dsp_volume_cfg::kMaxDb;
    }
    attDb_.store(db, std::memory_order_relaxed);
    attGain_.store(dbToGain(db), std::memory_order_relaxed);
}

float DspVolume::attenuationDb() const {
    return attDb_.load(std::memory_order_relaxed);
}

void DspVolume::setMute(bool mute) {
    mute_.store(mute, std::memory_order_relaxed);
}

bool DspVolume::muted() const {
    return mute_.load(std::memory_order_relaxed);
}

void DspVolume::resetState() {
    gCur_ = 0.0f;
    gTgt_ = 0.0f;
    step_ = 0.0f;
    rem_ = 0;
    silent_.store(true, std::memory_order_relaxed);
}

void DspVolume::forceMute() {
    mute_.store(true, std::memory_order_relaxed);
    resetState();
}

void DspVolume::requestForceMute() {
    mute_.store(true, std::memory_order_relaxed);
    forceReq_.store(true, std::memory_order_release);
}

bool DspVolume::isRamping() const {
    return rem_ > 0;
}

bool DspVolume::isSilent() const {
    return mute_.load(std::memory_order_relaxed) && silent_.load(std::memory_order_relaxed);
}

void DspVolume::process(int32_t* buf, int32_t frames) {
    if (buf == nullptr || frames <= 0) {
        return;
    }

    // [P47a] Запит миттєвого мʼюту з іншої задачі: скидаємо стан власника тут. Мʼют не чіпаємо:
    // якщо запитач уже встиг викликати setMute(false), далі піде плавне наростання з нуля.
    if (forceReq_.exchange(false, std::memory_order_acq_rel)) {
        resetState();
    }

    // Нова ціль? Перехід починається з поточного підсилення.
    const bool m = mute_.load(std::memory_order_relaxed);
    const float a = attGain_.load(std::memory_order_relaxed);
    const float tgt = m ? 0.0f : a;
    if (tgt != gTgt_) {
        const uint32_t ms = m ? dsp_volume_cfg::kMuteRampMs : dsp_volume_cfg::kRampMs;
        rem_ = framesFor(ms, hz_);
        gTgt_ = tgt;
        step_ = (tgt - gCur_) / static_cast<float>(rem_);
    }

    // Усталений 0 дБ без переходу: блок не чіпаємо (усі відліки лишаються точними).
    if (rem_ == 0 && gCur_ == 1.0f) {
        silent_.store(false, std::memory_order_relaxed);
        return;
    }

    float g = gCur_;
    uint32_t rem = rem_;
    const float step = step_;
    const float tgtG = gTgt_;

    int32_t* p = buf;
    for (int32_t f = 0; f < frames; ++f) {
        if (rem > 0) {
            g += step;
            if (--rem == 0) {
                g = tgtG;  // точне закінчення переходу
            }
        }
        p[0] = toInt32(static_cast<float>(p[0]) * g);
        p[1] = toInt32(static_cast<float>(p[1]) * g);
        p += 2;
    }

    gCur_ = g;
    rem_ = rem;
    silent_.store(rem == 0 && g == 0.0f, std::memory_order_relaxed);
}