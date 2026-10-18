#pragma once

// 5-смуговий графічний еквалайзер (Prompt 30; P45 — екземпляри, плавні коефіцієнти).
//
// Один екземпляр eq::Processor — незалежний ланцюжок: радіо (глобальний екземпляр за фасадом
// namespace eq) і line_path (власний екземпляр). Кожна ненульова смуга — peaking-biquad (RBJ)
// у структурі DF2T (TDF2), float32, каскад на кожен канал. Усі смуги 0 дБ або bypass: блок не
// чіпається (бітова ідентичність).
//
// ПОТОКИ (P45):
//  - Сеттери (setBandDb / setAllDb / setSampleRate / setBypass) і геттери викликаються з будь-якої
//    задачі. Цільові коефіцієнти рахуються ТУТ, у контексті викликача (double, pow/sin/cos), поза
//    гарячим шляхом, і публікуються через seqlock. Сеттери між собою серіалізуються мʼютексом.
//  - process() і reset() — ЛИШЕ з задачі-власника потоку (хук радіо / задача line_path). Без мʼютекса:
//    читання опублікованої цілі та лінійна інтерполяція коефіцієнтів за eq_cfg::kCoefRampMs.
//  - Інтерполяція між двома стійкими біквадами лишається стійкою: область стійкості (a1, a2) опукла.
//
// Збереження в NVS — НЕ тут (SettingsStore). Піни й I2S цей модуль не чіпає.
//
// Фасад namespace eq: API без змін (AppController, веб, SettingsStore, vu_pcm_hook, audio_player)
// делегує в глобальний екземпляр радіо.

#include <stdint.h>

#include <atomic>

#include "config/eq_config.h"

namespace eq {

namespace detail {

struct Coef {
    float b0, b1, b2, a1, a2;  // нормовані на a0
};

// Опублікована ціль: пишуть сеттери під мʼютексом, читає process() через seqlock.
struct Target {
    Coef     c[eq_cfg::kBandCount];
    uint8_t  on[eq_cfg::kBandCount];  // 1 = смуга з ненульовим підсиленням і в межах Найквіста
    uint8_t  snap;                    // 1 = миттєва зміна (нова fs): без інтерполяції, скидання стану
    uint32_t version;
    uint32_t rateHz;
};

}  // namespace detail

class Processor {
public:
    // tag — ідентифікатор у Serial-логу. Створює мʼютекс: викликати до старту задач
    // (статичний екземпляр або початок begin()).
    explicit Processor(const char* tag);
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    // --- сеттери / геттери: будь-яка задача ---
    void setSampleRate(uint32_t hz);
    uint32_t sampleRate() const;
    void setBandDb(uint8_t band, int8_t db);
    int8_t getBandDb(uint8_t band) const;
    void setAllDb(const int8_t gains[eq_cfg::kBandCount]);
    void getAllDb(int8_t out[eq_cfg::kBandCount]) const;
    void setBypass(bool on);
    bool bypass() const;
    uint32_t clipCount() const;   // відліки, насичені на виході, з останнього resetClipCount()
    void resetClipCount();

    // --- ЛИШЕ задача-власник потоку ---
    void reset();                                 // обнулити стан фільтрів (після enable / відновлення)
    void process(int32_t* buf, int32_t frames);   // стерео int32 L,R,L,R... НА МІСЦІ; frames — кадри

private:
    void lock() const;
    void unlock() const;
    void publishLocked(bool snap);                 // викликати під мʼютексом
    void readTarget(detail::Target& out) const;    // seqlock-читання
    void zeroBand(uint8_t b);
    void zeroStates();

    const char* tag_;
    mutable void* mutex_;                  // SemaphoreHandle_t: мʼютекс сеттерів
    std::atomic<uint32_t> rate_;           // поточна fs (читається без мʼютекса для швидкого шляху)
    std::atomic<bool>     bypass_;
    std::atomic<uint32_t> clips_;

    // Під мʼютексом сеттерів:
    int8_t   gains_[eq_cfg::kBandCount];
    uint32_t version_;

    // Seqlock опублікованої цілі:
    std::atomic<uint32_t> seq_;
    detail::Target        pub_;

    // Стан власника (process / reset):
    detail::Coef cur_[eq_cfg::kBandCount];    // поточні (інтерпольовані) коефіцієнти
    detail::Coef from_[eq_cfg::kBandCount];   // початок поточного переходу
    detail::Coef to_[eq_cfg::kBandCount];     // ціль поточного переходу
    uint8_t      wantOn_[eq_cfg::kBandCount];
    uint32_t     appliedVer_;
    uint32_t     rampTotal_;                  // довжина переходу, кадри (0 = переходу немає)
    uint32_t     rampDone_;
    bool         bypassSeen_;                 // був bypass: при виході з нього стан скидається
    float        z_[2][eq_cfg::kBandCount][2];  // [канал][смуга][z1, z2], DF2T
};

// ---------------------------------------------------------------------------
// Фасад: глобальний екземпляр радіо. Сигнатури незмінні (P30).
// ---------------------------------------------------------------------------

// Підсилення однієї смуги, дБ. band >= kBandCount ігнорується; db обрізається до
// eq_cfg::kGainMinDb..kGainMaxDb.
void setBandDb(uint8_t band, int8_t db);

// Поточне значення смуги (0 для неіснуючої).
int8_t getBandDb(uint8_t band);

// Усі смуги разом (один перерахунок). gains — масив eq_cfg::kBandCount значень.
void setAllDb(const int8_t gains[eq_cfg::kBandCount]);

// Копія поточного пресету в out (eq_cfg::kBandCount значень).
void getAllDb(int8_t out[eq_cfg::kBandCount]);

// Частота дискретизації потоку, Гц (з задачі плеєра). Значення поза межами ігноруються.
void setSampleRate(uint32_t hz);

// Остання прийнята частота дискретизації, Гц.
uint32_t sampleRate();

// Хук радіо: обробляє інтерліврований стерео-блок int32 НА МІСЦІ (задача плеєра).
void process(int32_t* buf, int32_t frames);

}  // namespace eq