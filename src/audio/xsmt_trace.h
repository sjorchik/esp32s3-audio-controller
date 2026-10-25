#pragma once

// Трасування послідовностей XSMT (Prompt X1). Усе під XSMT_TRACE (config/xsmt_config.h); при 0 макрос
// XSMT_EVT розкривається в порожній оператор — нульова вартість, нових задач немає.
//
// Формат: "[XSMT] t=<millis> <подія>". Викликати можна з будь-якої задачі (один Serial.write на рядок).
// Не викликати з критичних секцій і ISR.

#include "config/xsmt_config.h"

#if XSMT_TRACE

namespace xsmt_trace {

// printf-подібний друк однієї події.
void event(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Читає Serial і виконує команди xsmt.cfg / xsmt.set <ім'я> <мс>. Викликати з loop() (main.cpp).
// Інші рядки ігнорує (вони теж споживаються з Serial: див. «Відомі обмеження» у звіті).
void pollConsole();

}  // namespace xsmt_trace

#define XSMT_EVT(...) xsmt_trace::event(__VA_ARGS__)

#else

#define XSMT_EVT(...) \
  do {                \
  } while (0)

#endif
