#pragma once

// [Prompt 47d] ТИМЧАСОВИЙ ПСЕВДОНІМ. Константи adc_vu (порт I2S1, буфери, DMA, задача, коефіцієнт зняття DC,
// kAdcVuOffsetDb) ВИДАЛЕНО: калібрування VU перенесено в line_path_cfg::kVuOffsetDb / kVuDcAlpha
// (config/line_path_config.h). Лишено лише прапорець ADC_VU_ENABLE, від якого, за P37, залежить виведення MCLK
// (GPIO16) в audio_player.cpp (його не було серед вкладень). MCLK потрібен PCM1808 лише під час проходу, тож
// ADC_VU_ENABLE == LINE_PATH_ENABLE. ВИДАЛИТИ разом із правкою audio_player.cpp (#if ADC_VU_ENABLE ->
// #if LINE_PATH_ENABLE). Якщо audio_player.cpp використовує adc_vu_cfg::*, збірка підкаже.

#include <stdint.h>

#include "config/line_path_config.h"
#include "config/pins.h"

#define ADC_VU_ENABLE LINE_PATH_ENABLE
