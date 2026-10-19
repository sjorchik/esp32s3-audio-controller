#pragma once

// [Prompt 47d] ТИМЧАСОВИЙ ШИМ. Модуль adc_vu ВИДАЛЕНО (adc_vu.cpp більше немає): VU зовнішніх входів тепер
// виробляє line_path через VuSourceLinePath (audio/vu_source.h). Цей файл лишено тільки для того, щоб
// ui/screens.cpp (його не було серед вкладень P47d) зібрався без правок. ВИДАЛИТИ разом із правкою
// screens.cpp: замінити #include "audio/adc_vu.h" на "audio/vu_source.h", VuSourceAdc -> VuSourceLinePath.
//
// AdcVu::begin()/isReady(): захоплення АЦП більше не існує; повертають, чи доступний VU проходу
// (LINE_PATH_ENABLE && ENABLE_VU) — на випадок, якщо екран сам перевіряє isReady().

#include "audio/vu_source.h"
#include "config/adc_vu_config.h"  // ADC_VU_ENABLE: старий adc_vu.h підключав його, screens.cpp може на це спиратись
#include "config/features.h"
#include "config/line_path_config.h"

using VuSourceAdc = VuSourceLinePath;

class AdcVu {
public:
    static constexpr bool begin() { return LINE_PATH_ENABLE && ENABLE_VU; }
    static constexpr bool isReady() { return LINE_PATH_ENABLE && ENABLE_VU; }
};