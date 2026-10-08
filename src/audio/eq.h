#pragma once

// 5-смуговий графічний еквалайзер декодованого PCM радіо (Prompt 30).
//
// Працює всередині хука audio_process_i2s (audio/vu_pcm_hook.cpp, ядро 1, гарячий шлях):
// кожна ненульова смуга — peaking-biquad (RBJ) у float, смуги йдуть каскадом на кожен канал.
// Порядок у хуку: eq::process() -> output_trim::process() -> підрахунок VU.
// Усі смуги 0 дБ: блок не чіпається (бітова ідентичність).
//
// Потоки: setBandDb()/setAllDb()/setSampleRate()/get*() — з будь-якої задачі, без мʼютексів
// (атомарні слова); process() — ЛИШЕ з хука (задача плеєра), без логів, блокувань і millis().
// Коефіцієнти перераховуються всередині process() (у задачі хука) при зміні пресету або
// частоти дискретизації. Файл не включає Audio.h (Arduino.h — лише в .cpp для EQ_DEBUG).
//
// Збереження в NVS — НЕ тут (модуль не знає про Settings): див. SettingsStore (Settings::eqGainsDb,
// застосовується в SettingsStore::load()) і веб-обробник /api/eq.

#include <stdint.h>

#include "config/eq_config.h"

namespace eq {

// Підсилення однієї смуги, дБ. band >= kBandCount ігнорується; db обрізається до
// eq_cfg::kGainMinDb..kGainMaxDb.
void setBandDb(uint8_t band, int8_t db);

// Поточне значення смуги (0 для неіснуючої).
int8_t getBandDb(uint8_t band);

// Усі смуги разом (один перерахунок). gains — масив eq_cfg::kBandCount значень.
void setAllDb(const int8_t gains[eq_cfg::kBandCount]);

// Копія поточного пресету в out (eq_cfg::kBandCount значень).
void getAllDb(int8_t out[eq_cfg::kBandCount]);

// Частота дискретизації потоку, Гц (з задачі плеєра). Значення поза
// eq_cfg::kMinSampleRateHz..kMaxSampleRateHz ігноруються.
void setSampleRate(uint32_t hz);

// Остання прийнята частота дискретизації, Гц.
uint32_t sampleRate();

// Хук: обробляє інтерліврований стерео-блок int32 (L,R,L,R...) НА МІСЦІ (16-бітні дані
// вирівняні вліво, повна шкала 2^31). frames — кількість стереокадрів (пар L,R).
void process(int32_t* buf, int32_t frames);

}  // namespace eq
