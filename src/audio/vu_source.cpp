// VU-метр з декодованих PCM-семплів радіо (Prompt 17) і з проходу line_path (Prompt 47d).
//
// Обмін між ядрами БЕЗ мʼютекса (мʼютекс у гарячому шляху аудіо = зайва затримка):
//  - продюсер (ядро 1, аудіо-callback) лише підтримує «пік з моменту останнього
//    читання» (max) і лічильник блоків;
//  - споживач (display-задача) забирає пік атомарним exchange(0), тож кожен пік
//    потрапляє на екран рівно один раз, а між кадрами (~40 мс) не губиться короткий
//    transient: беремо максимум за ВЕСЬ інтервал, а не останній блок (~1-25 мс).
// Єдина «гонка»: споживач обнулив значення між load() і store() продюсера — тоді
// пропадає пік, не більший за попередній (малий візуальний ефект, один кадр).
// Продюсер — єдиний письменник пікових значень і лічильника, тому read-modify-write
// (CAS) не потрібен; std::atomic<uint32_t> на ESP32-S3 безблокувальний.
//
// Свіжість даних споживач визначає за лічильником блоків (а не за millis() у
// гарячому шляху аудіо): лічильник не змінюється kStaleMs -> потік не грає.
//
// [VU1] VU_MEASURE_MODE == 1 (RMS). Продюсер (publishEnergy) раз на блок додає суму квадратів і кількість
// кадрів у КУМУЛЯТИВНІ лічильники (64 біт, лише його стан) і публікує їх молодші 32 біти (sum >> kEnergyShift)
// під seqlock: seq непарний = іде запис. Продюсер не чекає (wait-free), споживач повторює читання, якщо
// знімок розірвано. Споживач НІЧОГО не скидає: бере різницю між двома знімками (за модулем 2^32), отже
// кілька споживачів (екран, lp.vu) не крадуть дані один в одного, а гонки «обнулив між load і store» немає.
// Середній квадрат = Δсума / Δкадри за вікно >= kRmsMinWindowMs; корінь і дБ — лише тут (ядро 0).

#include "audio/vu_source.h"
#include "audio/vu_math.h"

#include <Arduino.h>
#include <math.h>

#include <atomic>

#include "config/features.h"
#include "config/line_path_config.h"  // [P47d] LINE_PATH_ENABLE
#include "config/vu_config.h"
#include "config/vu_sens_config.h"  // [V1]

#if VU_DEBUG
#define VU_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define VU_LOG(...) \
    do {            \
    } while (0)
#endif

namespace {

std::atomic<uint32_t> s_peakL{0};
std::atomic<uint32_t> s_peakR{0};
std::atomic<uint32_t> s_blocks{0};
std::atomic<uint32_t> s_lastFrames{0};  // лише для діагностики в логу

// [Prompt 47d] Піки проходу line_path (зовнішні входи): окремий набір, єдиний письменник — задача line_path.
std::atomic<uint32_t> s_linePeakL{0};
std::atomic<uint32_t> s_linePeakR{0};
std::atomic<uint32_t> s_lineBlocks{0};

constexpr std::memory_order kRelaxed = std::memory_order_relaxed;


// [V1] Чутливість VU активного входу, дБ. Пише AppController, читають read() обох джерел (одна display-задача).
std::atomic<int8_t> s_sensDb{vu_sens_cfg::kDefaultDb};

// Шкала dBFS і балістика — в audio/vu_math.h (спільні для VuSourceDecodedPcm і VuSourceLinePath).
using vu_math::ballistics;
using vu_math::peakToNorm;

#if ENABLE_VU && VU_MEASURE_MODE == 1
// [VU1] Поштова скринька енергії (RMS): один екземпляр на джерело. Поля cum* — ЛИШЕ продюсер (звичайні
// змінні, 64 біт для суми); решта — публікація під seqlock.
struct EnergyMailbox {
    uint64_t cumL = 0;
    uint64_t cumR = 0;
    uint32_t cumFrames = 0;
    std::atomic<uint32_t> seq{0};  // парний = стабільно; blocks = seq / 2
    std::atomic<uint32_t> pubL{0};
    std::atomic<uint32_t> pubR{0};
    std::atomic<uint32_t> pubFrames{0};

    // Продюсер: wait-free, лише float / цілі, без double / sqrtf / log10f / malloc.
    void publish(float sumSqL, float sumSqR, uint32_t frames) {
        // `!(x < cap)` також перехоплює NaN / inf; від'ємної суми бути не може, але й вона дає 0.
        if (!(sumSqL < vu_cfg::kEnergySumCap)) sumSqL = vu_cfg::kEnergySumCap;
        if (!(sumSqR < vu_cfg::kEnergySumCap)) sumSqR = vu_cfg::kEnergySumCap;
        if (sumSqL < 0.0f) sumSqL = 0.0f;
        if (sumSqR < 0.0f) sumSqR = 0.0f;
        cumL += static_cast<uint64_t>(sumSqL);
        cumR += static_cast<uint64_t>(sumSqR);
        cumFrames += frames;
        const uint32_t s = seq.load(kRelaxed);  // єдиний письменник
        seq.store(s + 1u, kRelaxed);            // непарний: запис іде
        std::atomic_thread_fence(std::memory_order_release);
        pubL.store(static_cast<uint32_t>(cumL >> vu_cfg::kEnergyShift), kRelaxed);
        pubR.store(static_cast<uint32_t>(cumR >> vu_cfg::kEnergyShift), kRelaxed);
        pubFrames.store(cumFrames, kRelaxed);
        seq.store(s + 2u, std::memory_order_release);  // парний: знімок готовий
    }
};

struct EnergySnap {
    uint32_t blocks;  // кількість публікацій (за модулем 2^32)
    uint32_t sumL;    // кумулятивна сума квадратів >> kEnergyShift (за модулем 2^32)
    uint32_t sumR;
    uint32_t frames;  // кумулятивна кількість кадрів (за модулем 2^32)
};

// Споживач: цілісний знімок або false (продюсер усі kEnergyReadRetries спроб був посеред запису).
bool snapshot(const EnergyMailbox& mb, EnergySnap& out) {
    for (uint8_t i = 0; i < vu_cfg::kEnergyReadRetries; ++i) {
        const uint32_t s1 = mb.seq.load(std::memory_order_acquire);
        if (s1 & 1u) {
            continue;
        }
        out.sumL = mb.pubL.load(kRelaxed);
        out.sumR = mb.pubR.load(kRelaxed);
        out.frames = mb.pubFrames.load(kRelaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = mb.seq.load(kRelaxed);
        if (s1 == s2) {
            out.blocks = s1 >> 1;
            return true;
        }
    }
    return false;
}

#if ENABLE_VU
EnergyMailbox s_radioEnergy;
#endif
#if LINE_PATH_ENABLE
EnergyMailbox s_lineEnergy;
#endif

struct RmsMsgs {
    const char* flowing;
    const char* stalled;
    const char* noData;
};

// Середній квадрат за вікно (одиниці int16²) з різниць кумулятивних лічильників.
float windowMeanSquare(uint32_t dSum, uint32_t dFrames) {
    if (dFrames == 0) {
        return 0.0f;
    }
    return static_cast<float>(dSum) * static_cast<float>(1u << vu_cfg::kEnergyShift) / static_cast<float>(dFrames);
}

// Спільне тіло read() для обох джерел у режимі RMS. Логіка свіжості й балістика — як у піковому режимі;
// різниця лише в тому, ЩО подається на ballistics: msqToNorm(середній квадрат вікна, чутливість).
bool readRms(const EnergyMailbox& mb, VuRmsState& st, const RmsMsgs& msg, float& left, float& right) {
    (void)msg;  // при VU_DEBUG == 0 повідомлення не друкуються
    left = 0.0f;
    right = 0.0f;
    EnergySnap sn;
    if (!snapshot(mb, sn)) {
        // Продюсер саме пише: пропускаємо кадр, рівень лишається попереднім (lastReadMs не оновлюємо, dt накопичується).
        if (!st.init) {
            return false;
        }
        left = st.levelL;
        right = st.levelR;
        return true;
    }
    const uint32_t now = millis();

    float dtMs = 0.0f;
    if (!st.init || (now - st.lastReadMs) > vu_cfg::kResumeGapMs) {
        // Перший виклик або екран давно не опитував VU: старі рівні й накопичену енергію не показуємо.
        st.levelL = 0.0f;
        st.levelR = 0.0f;
        st.msqL = 0.0f;
        st.msqR = 0.0f;
        st.baseL = sn.sumL;
        st.baseR = sn.sumR;
        st.baseFrames = sn.frames;
        st.windowStartMs = now;
        st.lastBlocks = sn.blocks;
        st.lastChangeMs = now;
        st.sinceMs = now;
        st.flowing = false;
        st.init = true;
    } else {
        dtMs = static_cast<float>(now - st.lastReadMs);
    }
    st.lastReadMs = now;

    if (sn.blocks != st.lastBlocks) {
        st.lastBlocks = sn.blocks;
        st.lastChangeMs = now;
        if (!st.flowing) {
            st.flowing = true;
            st.everFlowed = true;
            VU_LOG("%s\n", msg.flowing);
        }
    } else if (now - st.lastChangeMs >= vu_cfg::kStaleMs) {
        // Потік не грає (або продюсер не викликається): ховаємо старе значення, вікно починаємо заново.
        st.levelL = 0.0f;
        st.levelR = 0.0f;
        st.msqL = 0.0f;
        st.msqR = 0.0f;
        st.baseL = sn.sumL;
        st.baseR = sn.sumR;
        st.baseFrames = sn.frames;
        st.windowStartMs = now;
        if (st.flowing) {
            st.flowing = false;
            VU_LOG("%s\n", msg.stalled);
        } else if (!st.everFlowed && !st.warnedNoData && now - st.sinceMs >= vu_cfg::kNoDataWarnMs) {
            st.warnedNoData = true;
            VU_LOG("%s\n", msg.noData);
        }
        return false;
    }

    // Вікно усереднення: від попереднього знімка, але не коротше kRmsMinWindowMs (інакше лишається msq
    // попереднього вікна, а нові кадри копляться далі — лічильники кумулятивні, тож нічого не губиться).
    if (now - st.windowStartMs >= vu_cfg::kRmsMinWindowMs) {
        const uint32_t dFrames = sn.frames - st.baseFrames;
        st.msqL = windowMeanSquare(sn.sumL - st.baseL, dFrames);
        st.msqR = windowMeanSquare(sn.sumR - st.baseR, dFrames);
        st.baseL = sn.sumL;
        st.baseR = sn.sumR;
        st.baseFrames = sn.frames;
        st.windowStartMs = now;
    }

    const float sens = static_cast<float>(s_sensDb.load(kRelaxed));  // [V1] чутливість VU активного входу, дБ
    st.levelL = ballistics(st.levelL, vu_math::msqToNorm(st.msqL, sens), dtMs);
    st.levelR = ballistics(st.levelR, vu_math::msqToNorm(st.msqR, sens), dtMs);
    left = st.levelL;
    right = st.levelR;
    return true;
}
#endif  // ENABLE_VU && VU_MEASURE_MODE == 1

}  // namespace

// [V1] Не виходить за межі vu_sens_cfg (clampDb); без логів і блокувань.
void VuSource::setSensitivityDb(int8_t db) {
    s_sensDb.store(vu_sens_cfg::clampDb(db), kRelaxed);
}

int8_t VuSource::sensitivityDb() {
    return s_sensDb.load(kRelaxed);
}

void VuSourceDecodedPcm::publishPeaks(uint32_t peakL, uint32_t peakR, uint32_t frames) {
#if ENABLE_VU
    if (peakL > s_peakL.load(kRelaxed)) {
        s_peakL.store(peakL, kRelaxed);
    }
    if (peakR > s_peakR.load(kRelaxed)) {
        s_peakR.store(peakR, kRelaxed);
    }
    s_lastFrames.store(frames, kRelaxed);
    s_blocks.store(s_blocks.load(kRelaxed) + 1u, kRelaxed);  // єдиний письменник
#else
    (void)peakL;
    (void)peakR;
    (void)frames;
#endif
}

// [VU1] Продюсер RMS радіо: викликається з хука audio_process_i2s (ядро 1) раз на блок.
void VuSourceDecodedPcm::publishEnergy(float sumSqL, float sumSqR, uint32_t frames) {
#if ENABLE_VU && VU_MEASURE_MODE == 1
    s_radioEnergy.publish(sumSqL, sumSqR, frames);
#else
    (void)sumSqL;
    (void)sumSqR;
    (void)frames;
#endif
}

bool VuSourceDecodedPcm::read(float& left, float& right) {
    left = 0.0f;
    right = 0.0f;
#if !ENABLE_VU
    return false;
#elif VU_MEASURE_MODE == 1
    // [VU1] RMS: середній квадрат за вікно між read() -> дБ -> та сама балістика.
    static const RmsMsgs kMsgs = {"[VU] PCM flowing (rms)", "[VU] PCM stalled",
                                  "[VU] no PCM blocks while playing - check VU_PCM_HOOK_STYLE / audio_process_i2s signature"};
    return readRms(s_radioEnergy, m_rms, kMsgs, left, right);
#else
    const uint32_t now = millis();
    const uint32_t blocks = s_blocks.load(kRelaxed);

    float dtMs = 0.0f;
    if (!m_init || (now - m_lastReadMs) > vu_cfg::kResumeGapMs) {
        // Перший виклик або екран давно не опитував VU (пауза, інший режим):
        // старі рівні й накопичений пік не показуємо.
        m_levelL = 0.0f;
        m_levelR = 0.0f;
        m_lastBlocks = blocks;
        m_lastChangeMs = now;
        m_sinceMs = now;
        m_flowing = false;
        m_init = true;
        s_peakL.exchange(0, kRelaxed);
        s_peakR.exchange(0, kRelaxed);
    } else {
        dtMs = static_cast<float>(now - m_lastReadMs);
    }
    m_lastReadMs = now;

    if (blocks != m_lastBlocks) {
        m_lastBlocks = blocks;
        m_lastChangeMs = now;
        if (!m_flowing) {
            m_flowing = true;
            m_everFlowed = true;
            VU_LOG("[VU] PCM flowing, last block %lu frames\n",
                   static_cast<unsigned long>(s_lastFrames.load(kRelaxed)));
        }
    } else if (now - m_lastChangeMs >= vu_cfg::kStaleMs) {
        // Потік не грає (або хук не викликається): ховаємо старе значення.
        m_levelL = 0.0f;
        m_levelR = 0.0f;
        s_peakL.exchange(0, kRelaxed);
        s_peakR.exchange(0, kRelaxed);
        if (m_flowing) {
            m_flowing = false;
            VU_LOG("[VU] PCM stalled\n");
        } else if (!m_everFlowed && !m_warnedNoData &&
                   now - m_sinceMs >= vu_cfg::kNoDataWarnMs) {
            m_warnedNoData = true;
            VU_LOG("[VU] no PCM blocks while playing - check VU_PCM_HOOK_STYLE / "
                   "audio_process_i2s signature\n");
        }
        return false;
    }

    const uint32_t pl = s_peakL.exchange(0, kRelaxed);
    const uint32_t pr = s_peakR.exchange(0, kRelaxed);
    const float sens = static_cast<float>(s_sensDb.load(kRelaxed));  // [V1] чутливість VU активного входу, дБ
    m_levelL = ballistics(m_levelL, peakToNorm(pl, sens), dtMs);
    m_levelR = ballistics(m_levelR, peakToNorm(pr, sens), dtMs);
    left = m_levelL;
    right = m_levelR;
    return true;
#endif
}

// ---------------------------------------------------------------------------
// [Prompt 47d] VuSourceLinePath: VU зовнішніх входів зі line_path.
// Логіка read(): пік «з моменту останнього читання» (exchange(0)),
// свіжість за лічильником блоків (kStaleMs), балістика з vu_math. Без логу у гарячому шляху.
// ---------------------------------------------------------------------------

void VuSourceLinePath::publishPeaks(uint32_t peakL, uint32_t peakR, uint32_t frames) {
#if ENABLE_VU && LINE_PATH_ENABLE
    (void)frames;
    if (peakL > s_linePeakL.load(kRelaxed)) {
        s_linePeakL.store(peakL, kRelaxed);
    }
    if (peakR > s_linePeakR.load(kRelaxed)) {
        s_linePeakR.store(peakR, kRelaxed);
    }
    s_lineBlocks.store(s_lineBlocks.load(kRelaxed) + 1u, kRelaxed);  // єдиний письменник
#else
    (void)peakL;
    (void)peakR;
    (void)frames;
#endif
}

// [VU1] Продюсер RMS проходу line_path: лише задача line_path (ядро 1), раз на блок.
void VuSourceLinePath::publishEnergy(float sumSqL, float sumSqR, uint32_t frames) {
#if ENABLE_VU && LINE_PATH_ENABLE && VU_MEASURE_MODE == 1
    s_lineEnergy.publish(sumSqL, sumSqR, frames);
#else
    (void)sumSqL;
    (void)sumSqR;
    (void)frames;
#endif
}

bool VuSourceLinePath::read(float& left, float& right) {
    left = 0.0f;
    right = 0.0f;
#if !ENABLE_VU || !LINE_PATH_ENABLE
    return false;
#elif VU_MEASURE_MODE == 1
    // [VU1] RMS: див. VuSourceDecodedPcm::read().
    static const RmsMsgs kMsgs = {"[VU] line path flowing (rms)", "[VU] line path stalled",
                                  "[VU] no line path blocks on external input - check line_path state / PCM1808"};
    return readRms(s_lineEnergy, m_rms, kMsgs, left, right);
#else
    const uint32_t now = millis();
    const uint32_t blocks = s_lineBlocks.load(kRelaxed);

    float dtMs = 0.0f;
    if (!m_init || (now - m_lastReadMs) > vu_cfg::kResumeGapMs) {
        m_levelL = 0.0f;
        m_levelR = 0.0f;
        m_lastBlocks = blocks;
        m_lastChangeMs = now;
        m_sinceMs = now;
        m_flowing = false;
        m_init = true;
        s_linePeakL.exchange(0, kRelaxed);
        s_linePeakR.exchange(0, kRelaxed);
    } else {
        dtMs = static_cast<float>(now - m_lastReadMs);
    }
    m_lastReadMs = now;

    if (blocks != m_lastBlocks) {
        m_lastBlocks = blocks;
        m_lastChangeMs = now;
        if (!m_flowing) {
            m_flowing = true;
            m_everFlowed = true;
            VU_LOG("[VU] line path flowing\n");
        }
    } else if (now - m_lastChangeMs >= vu_cfg::kStaleMs) {
        // Прохід не працює (інший режим, Stall, перемикання): ховаємо старе значення.
        m_levelL = 0.0f;
        m_levelR = 0.0f;
        s_linePeakL.exchange(0, kRelaxed);
        s_linePeakR.exchange(0, kRelaxed);
        if (m_flowing) {
            m_flowing = false;
            VU_LOG("[VU] line path stalled\n");
        } else if (!m_everFlowed && !m_warnedNoData && now - m_sinceMs >= vu_cfg::kNoDataWarnMs) {
            m_warnedNoData = true;
            VU_LOG("[VU] no line path blocks on external input - check line_path state / PCM1808\n");
        }
        return false;
    }

    const uint32_t pl = s_linePeakL.exchange(0, kRelaxed);
    const uint32_t pr = s_linePeakR.exchange(0, kRelaxed);
    const float sens = static_cast<float>(s_sensDb.load(kRelaxed));  // [V1] чутливість VU активного входу, дБ
    m_levelL = ballistics(m_levelL, peakToNorm(pl, sens), dtMs);
    m_levelR = ballistics(m_levelR, peakToNorm(pr, sens), dtMs);
    left = m_levelL;
    right = m_levelR;
    return true;
#endif
}

