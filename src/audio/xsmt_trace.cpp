// audio/xsmt_trace.cpp (Prompt X1): трасування XSMT і живе налаштування таймінгів. Компілюється лише при
// XSMT_TRACE == 1.

#include "config/xsmt_config.h"

#if XSMT_TRACE

#include "audio/xsmt_trace.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/pins.h"

namespace xsmt {
volatile uint32_t g_values[kParamCount] = {
    xsmt_cfg::kMuteLeadMs, xsmt_cfg::kClockSettleMs, xsmt_cfg::kUnmuteHoldMs,
    xsmt_cfg::kClockSwitchGapMs, xsmt_cfg::kRateWaitMaxMs, xsmt_cfg::kAdcSettleMs,
};
}  // namespace xsmt

namespace {

struct ParamInfo {
  const char* name;
  xsmt::Param id;
  uint32_t    def;
};

constexpr ParamInfo kParams[] = {
    {"mute_lead", xsmt::kPMuteLead, xsmt_cfg::kMuteLeadMs},
    {"clock_settle", xsmt::kPClockSettle, xsmt_cfg::kClockSettleMs},
    {"unmute_hold", xsmt::kPUnmuteHold, xsmt_cfg::kUnmuteHoldMs},
    {"clock_gap", xsmt::kPClockSwitchGap, xsmt_cfg::kClockSwitchGapMs},
    {"rate_wait", xsmt::kPRateWaitMax, xsmt_cfg::kRateWaitMaxMs},
    {"adc_settle", xsmt::kPAdcSettle, xsmt_cfg::kAdcSettleMs},
};
constexpr size_t kParamTotal = sizeof(kParams) / sizeof(kParams[0]);
static_assert(kParamTotal == xsmt::kParamCount, "param table out of sync");

void printCfg() {
  xsmt_trace::event("cfg: XSMT pin level=%d (RAM values, not saved; defaults in config/xsmt_config.h)",
                    digitalRead(pins::kXsmt));
  for (size_t i = 0; i < kParamTotal; ++i) {
    xsmt_trace::event("cfg: %s = %lu ms (default %lu)", kParams[i].name,
                      static_cast<unsigned long>(xsmt::g_values[kParams[i].id]),
                      static_cast<unsigned long>(kParams[i].def));
  }
}

void handleLine(const char* line) {
  if (strncmp(line, "xsmt.", 5) != 0) {
    return;
  }
  const char* c = line + 5;
  if (strcmp(c, "cfg") == 0) {
    printCfg();
    return;
  }
  if (strncmp(c, "set ", 4) == 0) {
    char          name[24];
    unsigned long ms = 0;
    if (sscanf(c + 4, "%23s %lu", name, &ms) != 2) {
      xsmt_trace::event("set: usage 'xsmt.set <name> <ms>' (names: mute_lead clock_settle unmute_hold clock_gap rate_wait adc_settle)");
      return;
    }
    if (ms > xsmt_cfg::kParamMaxMs) {
      xsmt_trace::event("set: %lu ms out of range 0..%lu", ms, static_cast<unsigned long>(xsmt_cfg::kParamMaxMs));
      return;
    }
    for (size_t i = 0; i < kParamTotal; ++i) {
      if (strcmp(name, kParams[i].name) == 0) {
        xsmt::g_values[kParams[i].id] = static_cast<uint32_t>(ms);
        xsmt_trace::event("set: %s = %lu ms", name, ms);
        return;
      }
    }
    xsmt_trace::event("set: unknown name '%s'", name);
    return;
  }
  xsmt_trace::event("commands: xsmt.cfg | xsmt.set <name> <ms>");
}

}  // namespace

namespace xsmt_trace {

void event(const char* fmt, ...) {
  char    body[xsmt_cfg::kEventLineMax];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  char      line[xsmt_cfg::kEventLineMax + 24];
  const int n = snprintf(line, sizeof(line), "[XSMT] t=%lu %s\n", static_cast<unsigned long>(millis()), body);
  if (n > 0) {
    const size_t len = (static_cast<size_t>(n) < sizeof(line)) ? static_cast<size_t>(n) : sizeof(line) - 1;
    Serial.write(reinterpret_cast<const uint8_t*>(line), len);
  }
}

void pollConsole() {
  static char   line[xsmt_cfg::kConsoleLineMax];
  static size_t n = 0;
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch == '\r' || ch == '\n') {
      if (n > 0) {
        line[n] = '\0';
        handleLine(line);
        n = 0;
      }
    } else if (n + 1 < sizeof(line)) {
      line[n++] = static_cast<char>(ch);
    }
  }
}

}  // namespace xsmt_trace

#endif  // XSMT_TRACE
