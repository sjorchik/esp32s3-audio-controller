#pragma once

// Цифровий рівень виходу декодера (Prompt 25): послаблення PCM у дБ ДО I2S.
//
// Працює всередині хука audio_process_i2s (audio/vu_pcm_hook.cpp, ядро 1, гарячий шлях):
// семпли блоку множаться на лінійний коефіцієнт у форматі Q16, який плавно (по кадрах)
// наближається до цілі. Лише послаблення: 0 дБ = блок не чіпається (бітова ідентичність).
// Гучність бібліотеки й формат I2S не змінюються.
//
// Потоки: setTargetDb()/targetDb() — з будь-якої задачі (одне 32-бітне слово, без мʼютекса);
// process() — ЛИШЕ з хука (задача плеєра), без логів, блокувань і millis().
// Публічний інтерфейс для решти проєкту — AudioPlayer::setOutputTrimDb()/outputTrimDb().

#include <stdint.h>

namespace output_trim {

// Нова ціль, дБ. Значення поза [kLevelMinDb, kLevelMaxDb] обрізається. Перехід плавний.
void setTargetDb(int8_t db);

// Остання встановлена ціль, дБ (початково station_level_cfg::kDefaultStationLevelDb).
int8_t targetDb();

// Хук: масштабує інтерліврований стерео-блок int32 (L,R,L,R...) НА МІСЦІ.
// frames — кількість стереокадрів (пар L,R).
void process(int32_t* buf, int32_t frames);

}  // namespace output_trim
