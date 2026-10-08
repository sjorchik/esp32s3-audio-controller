// VU-метр з декодованих PCM-семплів радіо (Prompt 17).
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

#include "audio/vu_source.h"
#include "audio/vu_math.h"

#include <Arduino.h>
#include <math.h>

#include <atomic>

#include "config/features.h"
#include "config/vu_config.h"

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

constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

// [Prompt 37] Шкала dBFS і балістика винесені в audio/vu_math.h (спільні з VuSourceAdc);
// код перенесено без змін.
using vu_math::ballistics;
using vu_math::peakToNorm;

}  // namespace

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

bool VuSourceDecodedPcm::read(float& left, float& right) {
    left = 0.0f;
    right = 0.0f;
#if !ENABLE_VU
    return false;
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
    m_levelL = ballistics(m_levelL, peakToNorm(pl), dtMs);
    m_levelR = ballistics(m_levelR, peakToNorm(pr), dtMs);
    left = m_levelL;
    right = m_levelR;
    return true;
#endif
}
