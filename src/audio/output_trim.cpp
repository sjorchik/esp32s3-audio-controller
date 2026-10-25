// Цифровий рівень виходу декодера (Prompt 25). Опис і правила потоків — у output_trim.h.
//
// ВАЖЛИВО: файл НЕ включає Audio.h (як і vu_pcm_hook.cpp) — тут лише чиста математика.

#include "audio/output_trim.h"

#include <stddef.h>

#include "config/station_level_config.h"

namespace {

namespace lc = station_level_cfg;

constexpr int32_t kUnity = 1 << 16;  // 0 дБ у Q16

// Лінійні коефіцієнти Q16 для kLevelMinDb..kLevelMaxDb (крок 1 дБ): round(65536 * 10^(dB/20)).
// Індекс = dB - kLevelMinDb. Таблиця замість powf(): без FPU у гарячому шляху й без залежності
// від libm.
constexpr int32_t kGainQ16[] = {
    4135,  4640,  5206,  5841,  6554,  7353,  8250,  9257,  10387, 11654, 13076, 14672, 16462,
    18471, 20724, 23253, 26090, 29274, 32846, 36854, 41350, 46396, 52057, 58409, 65536,
};
static_assert(sizeof(kGainQ16) / sizeof(kGainQ16[0]) ==
                  static_cast<size_t>(lc::kLevelMaxDb - lc::kLevelMinDb + 1),
              "gain table must cover kLevelMinDb..kLevelMaxDb");
static_assert(kGainQ16[lc::kLevelMaxDb - lc::kLevelMinDb] == kUnity, "0 dB must be unity");

// Крок зміни коефіцієнта за кадр: увесь діапазон за kTrimRampFrames кадрів (округлення вгору).
constexpr int32_t kStepQ16 = (kUnity + lc::kTrimRampFrames - 1) / lc::kTrimRampFrames;
static_assert(kStepQ16 >= 1, "ramp step must be at least 1");

int32_t gainForDb(int db) {
    if (db < lc::kLevelMinDb) db = lc::kLevelMinDb;
    if (db > lc::kLevelMaxDb) db = lc::kLevelMaxDb;
    return kGainQ16[db - lc::kLevelMinDb];
}

// Ціль: пишуть інші задачі, читає хук (по одному 32-бітному слову, атомарно на Xtensa).
volatile int32_t s_targetGain = kGainQ16[lc::kDefaultStationLevelDb - lc::kLevelMinDb];
volatile int8_t s_targetDb = static_cast<int8_t>(lc::kDefaultStationLevelDb);

// Поточний коефіцієнт: лише хук (задача плеєра). Початково = дефолт станції, тому до першого
// setTargetDb() звук не буде гучнішим за типовий.
int32_t s_curGain = kGainQ16[lc::kDefaultStationLevelDb - lc::kLevelMinDb];

inline int32_t scale(int32_t s, int32_t g) {
    // |s| <= 2^31, g <= 2^16: добуток у int64, округлення до найближчого; результат у межах int32.
    return static_cast<int32_t>((static_cast<int64_t>(s) * g + (1 << 15)) >> 16);
}

}  // namespace

void output_trim::setTargetDb(int8_t db) {
    int d = db;
    if (d < lc::kLevelMinDb) d = lc::kLevelMinDb;
    if (d > lc::kLevelMaxDb) d = lc::kLevelMaxDb;
    s_targetDb = static_cast<int8_t>(d);
    s_targetGain = gainForDb(d);
}

int8_t output_trim::targetDb() {
    return s_targetDb;
}

void output_trim::process(int32_t* buf, int32_t frames) {
    if (buf == nullptr || frames <= 0) {
        return;
    }
    int32_t cur = s_curGain;
    const int32_t tgt = s_targetGain;
    if (cur == tgt && cur >= kUnity) {
        return;  // 0 дБ і без переходу: блок не чіпаємо
    }
    int32_t* p = buf;
    if (cur == tgt) {
        // Усталений рівень: без гілок усередині циклу.
        for (int32_t i = 0; i < frames; ++i) {
            p[0] = scale(p[0], cur);
            p[1] = scale(p[1], cur);
            p += 2;
        }
        return;
    }
    for (int32_t i = 0; i < frames; ++i) {
        if (cur < tgt) {
            cur += kStepQ16;
            if (cur > tgt) cur = tgt;
        } else if (cur > tgt) {
            cur -= kStepQ16;
            if (cur < tgt) cur = tgt;
        }
        p[0] = scale(p[0], cur);
        p[1] = scale(p[1], cur);
        p += 2;
    }
    s_curGain = cur;
}
