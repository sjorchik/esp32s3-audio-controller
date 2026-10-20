// Цифрова гучність і мʼют (Prompt 45, доповнено Prompt 47a, P47d — processFloat, P48b — баланс). Опис і правила потоків — у dsp_volume.h.
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

// [P48b] Таблиця множників балансу: k дБ послаблення -> 10^(-k/20). k = 0 -> рівно 1.0f.
DspVolume::DspVolume() {
    balTab_[0] = 1.0f;
    for (int k = 1; k <= balance_cfg::kMaxDb; ++k) {
        balTab_[k] = dbToGain(-static_cast<float>(k));
    }
}

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

void DspVolume::setBalanceDb(int8_t db) {
    if (db < balance_cfg::kMinDb) db = balance_cfg::kMinDb;
    if (db > balance_cfg::kMaxDb) db = balance_cfg::kMaxDb;
    balDb_.store(db, std::memory_order_relaxed);
}

int8_t DspVolume::balanceDb() const {
    return balDb_.load(std::memory_order_relaxed);
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

// Спільна підготовка блоку. false = усталений 0 дБ без переходу: блок не чіпаємо (відліки точні).
bool DspVolume::prepare() {
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

    if (rem_ == 0 && gCur_ == 1.0f) {
        silent_.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void DspVolume::finish(float g, uint32_t rem) {
    gCur_ = g;
    rem_ = rem;
    silent_.store(rem == 0 && g == 0.0f, std::memory_order_relaxed);
}

// [P48b] Спільна підготовка балансу. Множники беруться з таблиці за |balanceDb|; послаблюється лівий канал при
// balanceDb > 0 (праворуч), правий при < 0; kSwapChannels міняє канали місцями. Нова ціль -> лінійний перехід
// від ПОТОЧНИХ множників. false = обидва множники рівно 1.0 і переходу немає: блок не чіпаємо.
bool DspVolume::balancePrepare() {
    const int8_t db = balDb_.load(std::memory_order_relaxed);
    float tl = 1.0f;
    float tr = 1.0f;
    if (db != 0) {
        const int a = db < 0 ? -static_cast<int>(db) : static_cast<int>(db);
        const float g = balTab_[a <= balance_cfg::kMaxDb ? a : balance_cfg::kMaxDb];
        const bool attenuateLeft = (db > 0) != balance_cfg::kSwapChannels;
        if (attenuateLeft) {
            tl = g;
        } else {
            tr = g;
        }
    }
    if (tl != blTgt_ || tr != brTgt_) {
        bRem_ = framesFor(balance_cfg::kRampMs, hz_);
        blTgt_ = tl;
        brTgt_ = tr;
        blStep_ = (tl - blCur_) / static_cast<float>(bRem_);
        brStep_ = (tr - brCur_) / static_cast<float>(bRem_);
    }
    return !(bRem_ == 0 && blCur_ == 1.0f && brCur_ == 1.0f);
}

void DspVolume::balanceApply(int32_t* buf, int32_t frames) {
    float l = blCur_;
    float r = brCur_;
    uint32_t rem = bRem_;
    const float sl = blStep_;
    const float sr = brStep_;
    const float tl = blTgt_;
    const float tr = brTgt_;

    int32_t* p = buf;
    for (int32_t f = 0; f < frames; ++f) {
        if (rem > 0) {
            l += sl;
            r += sr;
            if (--rem == 0) {
                l = tl;  // точне закінчення переходу
                r = tr;
            }
        }
        p[0] = toInt32(static_cast<float>(p[0]) * l);
        p[1] = toInt32(static_cast<float>(p[1]) * r);
        p += 2;
    }
    blCur_ = l;
    brCur_ = r;
    bRem_ = rem;
}

void DspVolume::balanceApplyFloat(float* buf, int32_t frames) {
    float l = blCur_;
    float r = brCur_;
    uint32_t rem = bRem_;
    const float sl = blStep_;
    const float sr = brStep_;
    const float tl = blTgt_;
    const float tr = brTgt_;

    float* p = buf;
    for (int32_t f = 0; f < frames; ++f) {
        if (rem > 0) {
            l += sl;
            r += sr;
            if (--rem == 0) {
                l = tl;
                r = tr;
            }
        }
        p[0] *= l;
        p[1] *= r;
        p += 2;
    }
    blCur_ = l;
    brCur_ = r;
    bRem_ = rem;
}

void DspVolume::process(int32_t* buf, int32_t frames) {
    if (buf == nullptr || frames <= 0) {
        return;
    }
    const bool vol = prepare();
    const bool bal = balancePrepare();  // [P48b]
    if (!vol && !bal) {
        return;
    }

    if (vol) {
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

        finish(g, rem);
    }
    if (bal) {
        balanceApply(buf, frames);  // [P48b] ПІСЛЯ гучності
    }
}

// [P47d] Той самий посемпловий ramp, без насичення.
void DspVolume::processFloat(float* buf, int32_t frames) {
    if (buf == nullptr || frames <= 0) {
        return;
    }
    const bool vol = prepare();
    const bool bal = balancePrepare();  // [P48b]
    if (!vol && !bal) {
        return;
    }

    if (vol) {
        float g = gCur_;
        uint32_t rem = rem_;
        const float step = step_;
        const float tgtG = gTgt_;

        float* p = buf;
        for (int32_t f = 0; f < frames; ++f) {
            if (rem > 0) {
                g += step;
                if (--rem == 0) {
                    g = tgtG;
                }
            }
            p[0] *= g;
            p[1] *= g;
            p += 2;
        }

        finish(g, rem);
    }
    if (bal) {
        balanceApplyFloat(buf, frames);  // [P48b] ПІСЛЯ гучності
    }
}
