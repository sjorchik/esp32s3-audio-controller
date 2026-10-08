// 5-смуговий еквалайзер радіо (Prompt 30). Опис і правила потоків — у eq.h.
//
// ВАЖЛИВО: файл НЕ включає Audio.h (як і vu_pcm_hook.cpp / output_trim.cpp) — чиста математика.

#include "audio/eq.h"

#include <atomic>
#include <math.h>
#include <string.h>

#if EQ_DEBUG
#include <Arduino.h>
#define EQ_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define EQ_LOG(...) \
    do {            \
    } while (0)
#endif

namespace {

constexpr uint8_t kN = eq_cfg::kBandCount;

// ---------------------------------------------------------------------------
// Спільний стан (пишуть інші задачі, читає хук)
// ---------------------------------------------------------------------------
std::atomic<int8_t> s_gainDb[kN] = {};
// Лічильник «пресет змінився»: хук перераховує коефіцієнти, коли він відрізняється від
// застосованого. Стартує з 1, а застосований = 0, тож перший блок завжди рахує коефіцієнти.
std::atomic<uint32_t> s_version{1};
std::atomic<uint32_t> s_rateHz{eq_cfg::kDefaultSampleRateHz};

// ---------------------------------------------------------------------------
// Стан хука (лише задача плеєра)
// ---------------------------------------------------------------------------
struct Coef {
    float b0, b1, b2, a1, a2;  // нормовані на a0
};

Coef s_coef[kN];
float s_z[2][kN][2];  // [канал][смуга][z1, z2] (форма DF2T)
uint8_t s_active[kN];
uint8_t s_activeCount = 0;
uint32_t s_appliedVersion = 0;
uint32_t s_appliedRate = 0;

int clampDb(int db) {
    if (db < eq_cfg::kGainMinDb) db = eq_cfg::kGainMinDb;
    if (db > eq_cfg::kGainMaxDb) db = eq_cfg::kGainMaxDb;
    return db;
}

// Peaking EQ (RBJ Audio EQ Cookbook). Коефіцієнти рахуємо в double (рідкісна подія,
// точність для низьких частот), зберігаємо в float.
Coef designPeaking(double f0, double dB, double fs) {
    const double kPi = 3.14159265358979323846;
    const double A = pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2.0 * eq_cfg::kQ);
    const double a0 = 1.0 + alpha / A;
    Coef c;
    c.b0 = static_cast<float>((1.0 + alpha * A) / a0);
    c.b1 = static_cast<float>((-2.0 * cw) / a0);
    c.b2 = static_cast<float>((1.0 - alpha * A) / a0);
    c.a1 = static_cast<float>((-2.0 * cw) / a0);
    c.a2 = static_cast<float>((1.0 - alpha / A) / a0);
    return c;
}

// Перерахунок за потреби (виклик лише з process()).
void refresh() {
    const uint32_t ver = s_version.load(std::memory_order_acquire);
    const uint32_t rate = s_rateHz.load(std::memory_order_relaxed);
    if (ver == s_appliedVersion && rate == s_appliedRate) {
        return;
    }
    const bool rateChanged = (rate != s_appliedRate);
    s_appliedVersion = ver;
    s_appliedRate = rate;
    if (rateChanged) {
        memset(s_z, 0, sizeof(s_z));  // інший потік — старий стан фільтрів не потрібен
    }
    s_activeCount = 0;
    for (uint8_t b = 0; b < kN; ++b) {
        const int db = s_gainDb[b].load(std::memory_order_relaxed);
        const double f0 = static_cast<double>(eq_cfg::kBandFreqHz[b]);
        const bool on = (db != 0) && (f0 <= eq_cfg::kMaxFreqRatio * static_cast<double>(rate));
        if (on) {
            s_coef[b] = designPeaking(f0, static_cast<double>(db), static_cast<double>(rate));
            s_active[s_activeCount++] = b;
        } else {
            // Вимкнена смуга: стан обнуляємо, щоб при вмиканні не було «хвоста».
            s_z[0][b][0] = s_z[0][b][1] = 0.0f;
            s_z[1][b][0] = s_z[1][b][1] = 0.0f;
        }
    }
}

// Найбільше float, що не перевищує int32 max (2147483647 у float не представляється).
constexpr float kOutMax = 2147483520.0f;
constexpr float kOutMin = -2147483648.0f;

inline int32_t toInt32(float y) {
    if (y > kOutMax) y = kOutMax;
    if (y < kOutMin) y = kOutMin;
    return static_cast<int32_t>(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Публічне API
// ---------------------------------------------------------------------------
void eq::setBandDb(uint8_t band, int8_t db) {
    if (band >= kN) {
        return;
    }
    const int8_t v = static_cast<int8_t>(clampDb(db));
    if (s_gainDb[band].exchange(v, std::memory_order_relaxed) != v) {
        s_version.fetch_add(1, std::memory_order_release);
        EQ_LOG("[EQ] band %u (%u Hz) = %d dB\n", static_cast<unsigned>(band),
               static_cast<unsigned>(eq_cfg::kBandFreqHz[band]), static_cast<int>(v));
    }
}

int8_t eq::getBandDb(uint8_t band) {
    if (band >= kN) {
        return 0;
    }
    return s_gainDb[band].load(std::memory_order_relaxed);
}

void eq::setAllDb(const int8_t gains[eq_cfg::kBandCount]) {
    if (gains == nullptr) {
        return;
    }
    bool changed = false;
    for (uint8_t b = 0; b < kN; ++b) {
        const int8_t v = static_cast<int8_t>(clampDb(gains[b]));
        if (s_gainDb[b].exchange(v, std::memory_order_relaxed) != v) {
            changed = true;
        }
    }
    if (changed) {
        s_version.fetch_add(1, std::memory_order_release);
        EQ_LOG("[EQ] preset %d %d %d %d %d dB\n", static_cast<int>(getBandDb(0)),
               static_cast<int>(getBandDb(1)), static_cast<int>(getBandDb(2)),
               static_cast<int>(getBandDb(3)), static_cast<int>(getBandDb(4)));
    }
}

void eq::getAllDb(int8_t out[eq_cfg::kBandCount]) {
    if (out == nullptr) {
        return;
    }
    for (uint8_t b = 0; b < kN; ++b) {
        out[b] = s_gainDb[b].load(std::memory_order_relaxed);
    }
}

void eq::setSampleRate(uint32_t hz) {
    if (hz < eq_cfg::kMinSampleRateHz || hz > eq_cfg::kMaxSampleRateHz) {
        return;
    }
    if (s_rateHz.exchange(hz, std::memory_order_relaxed) != hz) {
        EQ_LOG("[EQ] sample rate %lu Hz\n", static_cast<unsigned long>(hz));
    }
}

uint32_t eq::sampleRate() {
    return s_rateHz.load(std::memory_order_relaxed);
}

void eq::process(int32_t* buf, int32_t frames) {
    if (buf == nullptr || frames <= 0) {
        return;
    }
    refresh();
    const uint8_t n = s_activeCount;
    if (n == 0) {
        return;  // усі смуги 0 дБ (або вимкнені): блок не чіпаємо
    }

    // Локальні копії коефіцієнтів і станів активних смуг (у регістрах/стеку — швидше).
    Coef c[kN];
    float zl[kN][2];
    float zr[kN][2];
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t b = s_active[i];
        c[i] = s_coef[b];
        zl[i][0] = s_z[0][b][0];
        zl[i][1] = s_z[0][b][1];
        zr[i][0] = s_z[1][b][0];
        zr[i][1] = s_z[1][b][1];
    }

    int32_t* p = buf;
    for (int32_t f = 0; f < frames; ++f) {
        float xl = static_cast<float>(p[0]);
        float xr = static_cast<float>(p[1]);
        for (uint8_t i = 0; i < n; ++i) {
            const Coef& k = c[i];
            const float yl = k.b0 * xl + zl[i][0];
            zl[i][0] = k.b1 * xl - k.a1 * yl + zl[i][1];
            zl[i][1] = k.b2 * xl - k.a2 * yl;
            xl = yl;
            const float yr = k.b0 * xr + zr[i][0];
            zr[i][0] = k.b1 * xr - k.a1 * yr + zr[i][1];
            zr[i][1] = k.b2 * xr - k.a2 * yr;
            xr = yr;
        }
        p[0] = toInt32(xl);
        p[1] = toInt32(xr);
        p += 2;
    }

    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t b = s_active[i];
        s_z[0][b][0] = zl[i][0];
        s_z[0][b][1] = zl[i][1];
        s_z[1][b][0] = zr[i][0];
        s_z[1][b][1] = zr[i][1];
    }
}
