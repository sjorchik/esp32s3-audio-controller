// Прохід зовнішнього звуку PCM1808 -> ESP32-S3 -> PCM5102 (Prompt 44; P44b — життєвий цикл ресурсів;
// P45 — EQ і dsp_volume у ланцюжку блоку; P47b — виробничий API гучності, мʼюту користувача, EQ входу,
// помилок і неблокуючого старту; P47d — float-конвеєр і VU зовнішніх входів; P48b — цифровий баланс у DspVolume;
// PD1 — діагностика lp.cfg/raw/spec/bypass/tone). Опис і правила потоків — audio/line_path.h.
//
// Топологія (V2 з P43):
//   PCM1808 --DIN GPIO8--> I2S1 RX (slave) --[блок 128 кадрів]--> I2S1 TX (slave)
//   --DOUT--> GPIO18 --> PCM5102. BCLK GPIO15 / WS GPIO17 веде I2S0 бібліотеки (master, MCLK GPIO16).
//
// GPIO18 фізично одна: у радіо-режимі її веде I2S0 (ESP32-audioI2S, setPinout), у режимі проходу —
// I2S1 TX. Перемикання — лише GPIO-матрицею (esp_rom_gpio_connect_out_signal).
//
// ВАЖЛИВО про піни 15/17: вони водночас вихід I2S0 і вхід I2S1 (slave). Після init/enable повертаємо
// output-enable низькорівнево (gpio_ll). gpio_set_direction() не використовуємо.
//
// КОНВЕЄР БЛОКУ (P47d): RX int32 (24 біти зліва) -> float (×2^-31, точно) -> піки входу (lp.stat) ->
// EQ (processFloat) -> VU (піки після EQ, ДО гучності) -> гучність (processFloat) -> ОДНЕ насичення й
// перетворення в int32 -> піки виходу -> TX. Проміжних округлень і насичень немає. clipCount = лише
// фінальне насичення (після гучності). VU публікується в VuSourceLinePath (шкала int16 + kVuOffsetDb).
//
// МЬЮТИ (P47b): два джерела, одна функція applyMute(). Ефективний цифровий мʼют =
//   s_userMute || (s_state != Running).
// applyMute() викликається після КОЖНОЇ зміни s_state або s_userMute. Читання обох прапорців і запис
// в DspVolume — в одній критичній секції (s_muxMute), тож задача і зовнішній виклик не перевернуть
// мʼют один одному. Єдиний прямий розмʼют (раніше setMute(false) у pump) замінено на applyMute().
//
// ЖИТТЄВИЙ ЦИКЛ (P44b): start/requestStart створюють буфер блоку (внутрішня RAM) і задачу; задача
// першою дією створює канали I2S1 і сигналить результат (лише для блокуючого start). Невдача —
// lastError() і allocFailures++, XSMT low, GPIO18 на I2S0. returnToRadio() знищує канали й буфер.
//
// ЧИСТІ ПЕРЕХОДИ (P44b, перевірено на залізі lp.cycle): у TX пишуться нулі в settle-блоках і у вікні
// після XSMT low; канали вимикаються лише після цього вікна.
//
// LOG (P47b): кожен рядок виводиться через lineOut() — один Serial.write на рядок під мʼютексом.
// Задача line_path і Serial-тест більше не перемежовують байти.

#include "audio/line_path.h"
#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/audio_player.h"
#include "config/features.h"
#include "config/line_path_config.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

namespace cfg = line_path_cfg;

// Вивід у Serial: один рядок цілком, одним Serial.write, під мʼютексом. Рядки не перемежовуються
// між задачею line_path і Serial-тестом (причина втрачених \n у lp.*, див. звіт).
StaticSemaphore_t s_outBuf;
SemaphoreHandle_t s_outMux = xSemaphoreCreateMutexStatic(&s_outBuf);

void lineOut(const char* fmt, ...) {
  char    buf[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) {
    return;
  }
  size_t len = static_cast<size_t>(n);
  if (len >= sizeof(buf)) {  // обрізано: зберегти перенос рядка
    len = sizeof(buf) - 1;
    buf[len - 1] = '\n';
  }
  const bool locked = xSemaphoreTake(s_outMux, pdMS_TO_TICKS(100)) == pdTRUE;
  Serial.write(reinterpret_cast<const uint8_t*>(buf), len);
  if (locked) {
    xSemaphoreGive(s_outMux);
  }
}

#if LINE_PATH_DEBUG
#define LINE_LOG(...) lineOut(__VA_ARGS__)
#else
#define LINE_LOG(...) \
  do {                \
  } while (0)
#endif

[[maybe_unused]] size_t heapFree() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

[[maybe_unused]] size_t heapLargest() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

// Діагностика ресурсів для Serial-тесту.
struct DbgInfo {
  bool     alive;
  uint32_t stackFreeMin;
  uint32_t heapFreeBefore;
  uint32_t heapLargestBefore;
  uint32_t heapFreeAfter;
  uint32_t heapLargestAfter;
  uint32_t recoveries;
  float    vuPeakLeftDbfs;   // [P47d] пік VU (шкала екрана, з kVuOffsetDb) з останнього lp.reset, дБFS; -99 = тиша
  float    vuPeakRightDbfs;
};

[[maybe_unused]] const char* errorNameImpl(LinePathError e) {
  switch (e) {
    case LinePathError::None:                return "none";
    case LinePathError::AllocFailed:         return "alloc failed";
    case LinePathError::TaskCreateFailed:    return "task create failed";
    case LinePathError::ChannelCreateFailed: return "I2S1 channel create failed";
    case LinePathError::CreateTimeout:       return "resource create timeout";
    case LinePathError::RadioStopTimeout:    return "radio did not stop";
    case LinePathError::RadioRefused:        return "player refused fixed rate";
    case LinePathError::ClockFixFailed:      return "48 kHz not applied";
    case LinePathError::EnableFailed:        return "channel enable failed";
    case LinePathError::Stalled:             return "stalled (no RX blocks)";
  }
  return "?";
}

}  // namespace

#if LINE_PATH_ENABLE

#include "audio/dsp_volume.h"
#include "audio/eq.h"
#include "audio/vu_source.h"
#include "config/audio_player_config.h"
#include "config/dsp_volume_config.h"
#include "config/vu_config.h"
#include "driver/i2s_std.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"

static_assert(line_path_cfg::kXsmtMutedLevel == player_cfg::kXsmtMutedLevel &&
              line_path_cfg::kXsmtUnmutedLevel == player_cfg::kXsmtUnmutedLevel,
              "XSMT levels must match player_cfg");
static_assert(line_path_cfg::kVolMinDb == dsp_volume_cfg::kMinDb,
              "line path volume floor must equal dsp_volume floor");

namespace {

enum Req : uint8_t { kReqNone = 0, kReqStart, kReqStop, kReqRadio };

constexpr size_t kBufBytes = cfg::kBlockFrames * 2 * sizeof(int32_t);
// [P47d] Одне виділення: перша половина — int32 (RX / TX), друга — float-буфер конвеєра (той самий розмір).
static_assert(sizeof(float) == sizeof(int32_t), "float block buffer must match the int32 block size");
constexpr size_t kAllocBytes = 2 * kBufBytes;

constexpr float kToFloat = 1.0f / 2147483648.0f;    // 2^-31: int32 -> ±1.0 (степінь двійки: точно)
constexpr float kFromFloat = 2147483648.0f;         // 2^31
constexpr float kOutMax = 2147483520.0f;            // найбільше float <= INT32_MAX
constexpr float kOutMin = -2147483648.0f;

// --- DSP: екземпляри EQ і цифрової гучності; обробка — лише в задачі line_path ---
eq::Processor s_eqLine{"line"};
DspVolume     s_dsp;

// Критична секція для мʼютів (applyMute / setUserMute). Короткі, без блокувань всередині.
portMUX_TYPE s_muxMute = portMUX_INITIALIZER_UNLOCKED;

// --- Стан ---
volatile uint8_t       s_req = kReqNone;
volatile LinePathState s_state = LinePathState::Off;
volatile bool          s_muted = true;        // XSMT (лише для статистики / логу)
volatile bool          s_routed = false;
volatile bool          s_userMute = true;     // [P47b] мʼют користувача; типово увімкнений
volatile LinePathError s_lastError = LinePathError::None;
volatile uint32_t      s_rxOvf = 0;
volatile uint32_t      s_txOvf = 0;

// Життєвий цикл ресурсів.
bool              s_ready = false;
bool              s_opLock = false;          // серіалізує start / requestStart / stop / returnToRadio
volatile bool     s_taskAlive = false;
volatile bool     s_initAbandoned = false;
volatile bool     s_exit = false;
volatile int      s_initErr = 0;
SemaphoreHandle_t s_initSem = nullptr;
StaticSemaphore_t s_initSemBuf;
bool              s_rxOn = false;
bool              s_txOn = false;
i2s_chan_handle_t s_rx = nullptr;
i2s_chan_handle_t s_tx = nullptr;
TaskHandle_t      s_task = nullptr;
int32_t*          s_buf = nullptr;
float*            s_fbuf = nullptr;          // [P47d] друга половина того самого виділення

uint32_t s_settle = 0;
uint32_t s_lastGoodMs = 0;
uint32_t s_startReqMs = 0;
bool     s_recovering = false;
bool     s_pinsOk = true;

// Ін'єкція збою (lp.fault).
volatile uint32_t s_faultUntilMs = 0;
uint32_t          s_faultRx0 = 0;
uint32_t          s_faultTx0 = 0;

// Статистика.
uint32_t s_blocks = 0, s_readTimeouts = 0, s_readErrors = 0, s_writeTimeouts = 0;
uint32_t s_stalls = 0, s_starts = 0, s_maxProcessUs = 0, s_maxReadWaitUs = 0;
uint32_t s_initMs = 0, s_lastStartMs = 0, s_allocFailures = 0, s_recoveries = 0;
float    s_peakL = 0.0f, s_peakR = 0.0f;          // [P47d] пік входу, ±1.0 = повна шкала
float    s_peakOutL = 0.0f, s_peakOutR = 0.0f;    // пік виходу (після єдиного насичення)
uint32_t s_clips = 0;                             // [P47d] насичені відліки на виході (після гучності)

#if ENABLE_VU
// VU зовнішніх входів (P47d). Стан виміру живе лише в задачі line_path.
float    s_vuScale = 32768.0f;                    // int16-шкала × 10^(kVuOffsetDb/20); задається в begin()
float    s_vuDcL = 0.0f, s_vuDcR = 0.0f;
bool     s_vuDcInit = false;
uint32_t s_vuMaxL = 0, s_vuMaxR = 0;              // макс. опублікований пік з lp.reset (для lp.stat)
#endif

// Діагностика пам'яті / стека.
uint32_t s_heapFreeBefore = 0, s_heapLargestBefore = 0, s_heapFreeAfter = 0, s_heapLargestAfter = 0;
uint32_t s_stackFreeMin = 0;

// ===========================================================================
// [PD1] ДІАГНОСТИКА
// ===========================================================================
#if LINE_PATH_DIAG
volatile bool s_diagBypass = false;
volatile bool s_diagToneActive = false;
volatile float s_diagToneHz = 0.0f;
volatile float s_diagToneAmp = 0.0f;
float s_diagTonePhaseL = 0.0f;
float s_diagTonePhaseR = 0.0f;
float s_diagTonePhaseStep = 0.0f;

// Лічильники для lp.cfg
volatile uint32_t s_diagRxFrames = 0;
volatile uint32_t s_diagTxFrames = 0;
#endif

// [PD1] Алгоритм Герцеля для одного частотного біна.
struct GoertzelState {
  float s0, s1, s2;
  float coef;
};

inline void goertzelInit(GoertzelState& g, float freq, float fs) {
  g.s0 = g.s1 = g.s2 = 0.0f;
  float k = 0.5f + (freq * 1024.0f / fs); // N = 1024 (або довжина блоку)
  float w = (2.0f * 3.14159265358979323846f * k) / 1024.0f;
  g.coef = 2.0f * cosf(w);
}

inline void goertzelProcess(GoertzelState& g, float sample) {
  g.s0 = sample + g.coef * g.s1 - g.s2;
  g.s2 = g.s1;
  g.s1 = g.s0;
}

inline float goertzelMagnitudeDb(GoertzelState& g) {
  float mag2 = g.s1 * g.s1 + g.s2 * g.s2 - g.coef * g.s1 * g.s2;
  if (mag2 <= 1e-10f) return -99.0f;
  // Нормалізація: mag = sqrt(mag2) / (N/2). N=1024.
  // dBFS = 20 * log10(mag).
  float mag = sqrtf(mag2) / 512.0f;
  if (mag > 1.0f) mag = 1.0f;
  return 20.0f * log10f(mag);
}

// Глобальний стан для lp.spec (працює лише під час виконання команди)
volatile bool s_specRunning = false;
volatile uint32_t s_specEndMs = 0;
GoertzelState s_specL[line_path_diag_cfg::kGoertzelCount];
GoertzelState s_specR[line_path_diag_cfg::kGoertzelCount];
uint32_t s_specBlocks = 0;

// ---------------------------------------------------------------------------
// Мʼюти
// ---------------------------------------------------------------------------

// Ефективний цифровий мʼют = мʼют користувача АБО стан, відмінний від Running.
// Викликати ПІСЛЯ кожної зміни s_state або s_userMute. Читання і запис — в одній критичній секції.
void applyMute() {
  portENTER_CRITICAL(&s_muxMute);
  const bool eff = s_userMute || (s_state != LinePathState::Running);
  s_dsp.setMute(eff);
  portEXIT_CRITICAL(&s_muxMute);
}

// quiet = true: лише рівень піна й прапорець, без логу.
void muteDac(bool mute, bool quiet = false) {
  digitalWrite(pins::kXsmt, mute ? cfg::kXsmtMutedLevel : cfg::kXsmtUnmutedLevel);
  if (mute != s_muted && !quiet) {
    LINE_LOG("[LINE] XSMT %s\n", mute ? "mute" : "unmute");
  }
  s_muted = mute;
}

// ---------------------------------------------------------------------------
// Допоміжне: піни, маршрут GPIO18, канали
// ---------------------------------------------------------------------------

// Повернути output-enable BCLK/WS; true — обидва справді «на виході».
bool restorePinOutputs() {
  gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sBclk));
  gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sWs));
  gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sBclk));
  gpio_ll_input_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sWs));
  const bool bclkOut = ((GPIO.enable >> pins::kI2sBclk) & 1u) != 0;
  const bool wsOut = ((GPIO.enable >> pins::kI2sWs) & 1u) != 0;
  return bclkOut && wsOut;
}

// GPIO18: true — на I2S1 TX (прохід), false — назад на I2S0 TX (радіо).
void routeDout(bool toLine) {
  const uint32_t sig = toLine ? I2S1O_SD_OUT_IDX : I2S0O_SD_OUT_IDX;
  gpio_ll_output_enable(&GPIO, static_cast<gpio_num_t>(pins::kI2sDout));
  esp_rom_gpio_connect_out_signal(pins::kI2sDout, sig, false, false);
  s_routed = toLine;
  LINE_LOG("[LINE] DOUT GPIO%d -> %s\n", pins::kI2sDout, toLine ? "I2S1 TX (line)" : "I2S0 TX (radio)");
}

IRAM_ATTR bool onRxOvf(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  s_rxOvf = s_rxOvf + 1;
  return false;
}

IRAM_ATTR bool onTxOvf(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  s_txOvf = s_txOvf + 1;
  return false;
}

void disableChannels() {
  if (s_txOn) {
    i2s_channel_disable(s_tx);
    s_txOn = false;
  }
  if (s_rxOn) {
    i2s_channel_disable(s_rx);
    s_rxOn = false;
  }
}

// Створити дуплексну пару каналів I2S1 (slave). Виконує ЗАДАЧА line_path. GPIO18 не чіпає.
esp_err_t createChannels() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(cfg::kI2sPort),
                                                     I2S_ROLE_SLAVE);
  cc.dma_desc_num = cfg::kDmaDescCount;
  cc.dma_frame_num = cfg::kDmaFrameCount;
  cc.auto_clear = true;  // без даних TX шле нулі
  esp_err_t e = i2s_new_channel(&cc, &s_tx, &s_rx);
  if (e != ESP_OK) {
    s_tx = nullptr;
    s_rx = nullptr;
    return e;
  }

  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg::kFsHz);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = static_cast<gpio_num_t>(pins::kI2sBclk);
  sc.gpio_cfg.ws = static_cast<gpio_num_t>(pins::kI2sWs);
  sc.gpio_cfg.din = static_cast<gpio_num_t>(pins::kI2sDin);
  sc.gpio_cfg.dout = I2S_GPIO_UNUSED;

  e = i2s_channel_init_std_mode(s_rx, &sc);
  if (e == ESP_OK) {
    e = i2s_channel_init_std_mode(s_tx, &sc);
  }
  if (e != ESP_OK) {
    i2s_del_channel(s_rx);
    i2s_del_channel(s_tx);
    s_rx = nullptr;
    s_tx = nullptr;
    restorePinOutputs();
    return e;
  }

  i2s_event_callbacks_t rxCb = {};
  rxCb.on_recv_q_ovf = onRxOvf;
  i2s_channel_register_event_callback(s_rx, &rxCb, nullptr);

  i2s_event_callbacks_t txCb = {};
  txCb.on_send_q_ovf = onTxOvf;
  i2s_channel_register_event_callback(s_tx, &txCb, nullptr);

  s_pinsOk = restorePinOutputs();
  return ESP_OK;
}

// Знищити канали і гарантувати, що I2S0 далі веде BCLK/WS.
void destroyChannels() {
  disableChannels();
  if (s_tx != nullptr) {
    i2s_del_channel(s_tx);
    s_tx = nullptr;
  }
  if (s_rx != nullptr) {
    i2s_del_channel(s_rx);
    s_rx = nullptr;
  }
  restorePinOutputs();
  esp_rom_gpio_connect_out_signal(pins::kI2sBclk, I2S0O_BCK_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(pins::kI2sWs, I2S0O_WS_OUT_IDX, false, false);
}

void freeBuffer() {
  if (s_buf != nullptr) {
    heap_caps_free(s_buf);
    s_buf = nullptr;
  }
  s_fbuf = nullptr;
}

// Скинути стан DSP і виміру VU (старт і відновлення після Stall). Лише задача line_path.
void resetDsp() {
  s_eqLine.reset();
#if ENABLE_VU
  s_vuDcInit = false;
#endif
}

// ---------------------------------------------------------------------------
// Обробка блоку: вхід -> EQ -> dsp_volume -> вихід
// ---------------------------------------------------------------------------

#if ENABLE_VU
// VU: пік по блоку ПІСЛЯ EQ і ДО гучності, у шкалі int16 з калібруванням kVuOffsetDb; зняття DC лише для виміру.
void publishVu(const float* f, int32_t n) {
  float dcL = s_vuDcL;
  float dcR = s_vuDcR;
  if (!s_vuDcInit) {  // перший блок: без стрибка від нульового стану фільтра
    dcL = f[0];
    dcR = f[1];
    s_vuDcInit = true;
  }
  const float a = cfg::kVuDcAlpha;
  float       pl = 0.0f;
  float       pr = 0.0f;
  for (int32_t i = 0; i < n; ++i) {
    const float xl = f[0];
    const float xr = f[1];
    f += 2;
    dcL += a * (xl - dcL);
    dcR += a * (xr - dcR);
    const float yl = fabsf(xl - dcL);
    const float yr = fabsf(xr - dcR);
    if (yl > pl) pl = yl;
    if (yr > pr) pr = yr;
  }
  s_vuDcL = dcL;
  s_vuDcR = dcR;
  float vl = pl * s_vuScale;
  float vr = pr * s_vuScale;
  if (vl > cfg::kVuPeakCap) vl = cfg::kVuPeakCap;
  if (vr > cfg::kVuPeakCap) vr = cfg::kVuPeakCap;
  const uint32_t ul = static_cast<uint32_t>(vl);
  const uint32_t ur = static_cast<uint32_t>(vr);
  if (ul > s_vuMaxL) s_vuMaxL = ul;
  if (ur > s_vuMaxR) s_vuMaxR = ur;
  VuSourceLinePath::publishPeaks(ul, ur, static_cast<uint32_t>(n));
}
#endif  // ENABLE_VU

// Обробка блоку (P47d): float-конвеєр з одним насиченням у кінці.
void processBlock(size_t frames) {
  const int32_t n = static_cast<int32_t>(frames);
  float*        f = s_fbuf;
  
#if LINE_PATH_DIAG
  s_diagRxFrames += n;
#endif

  // 1. int32 -> float (точно: 24 значущі біти) і піки входу (до EQ).
  float pl = 0.0f;
  float pr = 0.0f;
  {
    const int32_t* in = s_buf;
    float*         o = f;
    
#if LINE_PATH_DIAG
    // [PD1] Генерація тону замість RX
    if (s_diagToneActive) {
      for (int32_t i = 0; i < n; ++i) {
        float l = sinf(s_diagTonePhaseL) * s_diagToneAmp;
        float r = sinf(s_diagTonePhaseR) * s_diagToneAmp;
        s_diagTonePhaseL += s_diagTonePhaseStep;
        s_diagTonePhaseR += s_diagTonePhaseStep;
        if (s_diagTonePhaseL > 6.2831853f) s_diagTonePhaseL -= 6.2831853f;
        if (s_diagTonePhaseR > 6.2831853f) s_diagTonePhaseR -= 6.2831853f;
        o[0] = l;
        o[1] = r;
        o += 2;
      }
    } else 
#endif
    {
      for (int32_t i = 0; i < n; ++i) {
        const float l = static_cast<float>(in[0]) * kToFloat;
        const float r = static_cast<float>(in[1]) * kToFloat;
        in += 2;
        o[0] = l;
        o[1] = r;
        o += 2;
      }
    }
    
    // Піки та Goertzel
    o = f;
    for (int32_t i = 0; i < n; ++i) {
      float l = o[0];
      float r = o[1];
      o += 2;
      const float al = fabsf(l);
      const float ar = fabsf(r);
      if (al > pl) pl = al;
      if (ar > pr) pr = ar;
      
#if LINE_PATH_DIAG
      if (s_specRunning) {
        for (size_t k = 0; k < line_path_diag_cfg::kGoertzelCount; ++k) {
          goertzelProcess(s_specL[k], l);
          goertzelProcess(s_specR[k], r);
        }
      }
#endif
    }
  }
  if (pl > s_peakL) s_peakL = pl;
  if (pr > s_peakR) s_peakR = pr;

#if LINE_PATH_DIAG
  // [PD1] Bypass: пропускаємо EQ, VU, Volume, Balance
  if (s_diagBypass) {
    // Перетворюємо float назад в int32 для TX
    const float* in = f;
    int32_t*     o = s_buf;
    for (int32_t i = 0; i < n; ++i) {
      float yl = in[0] * kFromFloat;
      float yr = in[1] * kFromFloat;
      in += 2;
      if (yl > kOutMax) yl = kOutMax;
      else if (yl < kOutMin) yl = kOutMin;
      if (yr > kOutMax) yr = kOutMax;
      else if (yr < kOutMin) yr = kOutMin;
      o[0] = static_cast<int32_t>(yl >= 0.0f ? yl + 0.5f : yl - 0.5f);
      o[1] = static_cast<int32_t>(yr >= 0.0f ? yr + 0.5f : yr - 0.5f);
      o += 2;
    }
    // Піки виходу
    float ol = pl, orr = pr;
    if (ol > s_peakOutL) s_peakOutL = ol;
    if (orr > s_peakOutR) s_peakOutR = orr;
    
    s_diagTxFrames += n;
    return; // Пропускаємо DSP
  }
#endif

  // 2. EQ, 3. VU (після EQ, до гучності), 4. гучність — усе у float, без насичення.
  s_eqLine.processFloat(f, n);
#if ENABLE_VU
  publishVu(f, n);
#endif
  s_dsp.processFloat(f, n);

  // 5. Єдине насичення й перетворення в int32 (округлення до найближчого); піки виходу.
  uint32_t clips = 0;
  float    ol = 0.0f;
  float    orr = 0.0f;
  {
    const float* in = f;
    int32_t*     o = s_buf;
    for (int32_t i = 0; i < n; ++i) {
      float yl = in[0] * kFromFloat;
      float yr = in[1] * kFromFloat;
      in += 2;
      if (yl > kOutMax) {
        yl = kOutMax;
        ++clips;
      } else if (yl < kOutMin) {
        yl = kOutMin;
        ++clips;
      }
      if (yr > kOutMax) {
        yr = kOutMax;
        ++clips;
      } else if (yr < kOutMin) {
        yr = kOutMin;
        ++clips;
      }
      const float al = fabsf(yl);
      const float ar = fabsf(yr);
      if (al > ol) ol = al;
      if (ar > orr) orr = ar;
      o[0] = static_cast<int32_t>(yl >= 0.0f ? yl + 0.5f : yl - 0.5f);
      o[1] = static_cast<int32_t>(yr >= 0.0f ? yr + 0.5f : yr - 0.5f);
      o += 2;
    }
  }
  ol *= kToFloat;
  orr *= kToFloat;
  if (ol > s_peakOutL) s_peakOutL = ol;
  if (orr > s_peakOutR) s_peakOutR = orr;
  if (clips != 0) {
    s_clips = s_clips + clips;
  }
  
#if LINE_PATH_DIAG
  s_diagTxFrames += n;
#endif
}

// Нема даних: перевіряємо аварійний мʼют.
void onNoData(uint32_t now) {
  const LinePathState st = s_state;
  if (st != LinePathState::Starting && st != LinePathState::Running) {
    return;
  }
  if (now - s_lastGoodMs < cfg::kStallMs) {
    return;
  }
  s_dsp.forceMute();   // миттєвий мʼют: блоків немає, перехід би не просунувся
  muteDac(true);
  s_state = LinePathState::Stalled;
  applyMute();
  s_stalls = s_stalls + 1;
  s_lastError = LinePathError::Stalled;
  LINE_LOG("[LINE] STALL: no I2S1 RX blocks for %lu ms - muted (check PCM1808 / clocks)\n",
           static_cast<unsigned long>(now - s_lastGoodMs));
}

void writeTx(size_t bytes) {
  size_t          wrote = 0;
  const esp_err_t we = i2s_channel_write(s_tx, s_buf, bytes, &wrote, cfg::kWriteTimeoutMs);
  if (we != ESP_OK || wrote != bytes) {
    s_writeTimeouts = s_writeTimeouts + 1;
  }
}

// Один цикл: читання -> (EQ, гучність) -> запис. true — блок отримано.
bool pump() {
  // Ін'єкція збою (lp.fault): RX не споживається.
  if (s_faultUntilMs != 0) {
    if (static_cast<int32_t>(millis() - s_faultUntilMs) < 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      onNoData(millis());
      return false;
    }
    s_faultUntilMs = 0;
    LINE_LOG("[LINE] FAULT injection over (RX/TX overflows during it: rx +%lu, tx +%lu)\n",
             static_cast<unsigned long>(s_rxOvf - s_faultRx0),
             static_cast<unsigned long>(s_txOvf - s_faultTx0));
  }

  size_t        got = 0;
  const int64_t t0 = esp_timer_get_time();
  const esp_err_t e = i2s_channel_read(s_rx, s_buf, kBufBytes, &got, cfg::kReadTimeoutMs);
  const uint32_t waitUs = static_cast<uint32_t>(esp_timer_get_time() - t0);
  if (waitUs > s_maxReadWaitUs) s_maxReadWaitUs = waitUs;

  const uint32_t now = millis();
  if (e != ESP_OK || got < 2 * sizeof(int32_t)) {
    if (e == ESP_ERR_TIMEOUT) {
      s_readTimeouts = s_readTimeouts + 1;
    } else {
      s_readErrors = s_readErrors + 1;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    onNoData(now);
    return false;
  }

  const uint32_t gapMs = now - s_lastGoodMs;
  s_lastGoodMs = now;

  bool logBack = false;
  if (s_state == LinePathState::Stalled) {
    s_state = LinePathState::Starting;
    s_settle = cfg::kSettleBlocks;
    s_recovering = true;
    resetDsp();
    applyMute();  // Starting: мʼют лишається
    logBack = true;
  }

  const size_t frames = got / (2 * sizeof(int32_t));
  const size_t bytes = frames * 2 * sizeof(int32_t);

  if (s_settle > 0) {
    --s_settle;
    memset(s_buf, 0, bytes);
    writeTx(bytes);
    if (logBack) {
      LINE_LOG("[LINE] RX data is back after %lu ms - re-settling\n", static_cast<unsigned long>(gapMs));
    }
    return true;
  }

  bool justStarted = false;
  bool justRecovered = false;

  if (s_state == LinePathState::Starting) {
    // Перший стабільний блок. Гучність з нуля (ramp піде від нуля). Мʼют користувача НЕ знімаємо:
    // applyMute() лишить мʼют, якщо користувач його увімкнув.
    s_dsp.forceMute();
    muteDac(false, true);
    s_state = LinePathState::Running;
    applyMute();
    if (s_recovering) {
      s_recovering = false;
      s_recoveries = s_recoveries + 1;
      justRecovered = true;
    } else {
      s_starts = s_starts + 1;
      s_lastStartMs = millis() - s_startReqMs;
      justStarted = true;
    }
  }

  const int64_t t1 = esp_timer_get_time();
  processBlock(frames);
  writeTx(bytes);
  const uint32_t procUs = static_cast<uint32_t>(esp_timer_get_time() - t1);
  if (procUs > s_maxProcessUs) s_maxProcessUs = procUs;
  s_blocks = s_blocks + 1;

  if (justStarted) {
    LINE_LOG("[LINE] running: XSMT unmute, volume %.1f dB, user mute %d, start took %lu ms%s\n",
             static_cast<double>(s_dsp.attenuationDb()), s_userMute ? 1 : 0,
             static_cast<unsigned long>(s_lastStartMs), s_pinsOk ? "" : ", WARNING BCLK/WS output enable MISSING");
  } else if (justRecovered) {
    LINE_LOG("[LINE] recovered: XSMT unmute, volume %.1f dB, user mute %d\n",
             static_cast<double>(s_dsp.attenuationDb()), s_userMute ? 1 : 0);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Послідовності
// ---------------------------------------------------------------------------

// Зняти фіксацію 48 кГц (лише при Idle радіо). Без повернення помилки: лише лог.
void releaseFixedRate() {
  if (!AudioPlayer::setFixedOutputRate(false)) {
    LINE_LOG("[LINE] radio: player refused to release fixed rate (not Idle?)\n");
    return;
  }
  const uint32_t c0 = millis();
  while (AudioPlayer::fixedOutputRate()) {
    if (millis() - c0 > cfg::kClockApplyWaitMs) {
      LINE_LOG("[LINE] radio: fixed rate not released\n");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void doStop() {
  s_faultUntilMs = 0;
  if (!s_rxOn && !s_txOn) {
    s_state = LinePathState::Off;
    applyMute();
    return;
  }
  s_state = LinePathState::Stopping;
  applyMute();  // плавний спад (dsp_volume, kMuteRampMs)
  const uint32_t t0 = millis();
  while (!s_dsp.isSilent() &&
         millis() - t0 < dsp_volume_cfg::kMuteRampMs * 3 + cfg::kReadTimeoutMs) {
    pump();
  }
  s_dsp.forceMute();  // на випадок таймауту: гарантовано тиша
  muteDac(true);
  const uint32_t w0 = millis();
  do {
    pump();
  } while (millis() - w0 < cfg::kXsmtMuteSettleMs);
  disableChannels();
  s_state = LinePathState::Off;
  applyMute();
  
#if LINE_PATH_DIAG
  s_diagBypass = false;
  s_diagToneActive = false;
  s_diagToneHz = 0.0f;
  s_specRunning = false;
#endif
  
  LINE_LOG("[LINE] stopped (DOUT %s)\n", s_routed ? "still on I2S1" : "on I2S0");
}

void doRadio() {
  if (s_state != LinePathState::Off) {
    doStop();
  }
  muteDac(true);
  s_dsp.forceMute();
  routeDout(false);
  releaseFixedRate();
  destroyChannels();
  freeBuffer();
  s_exit = true;
  applyMute();
  
#if LINE_PATH_DIAG
  s_diagBypass = false;
  s_diagToneActive = false;
  s_diagToneHz = 0.0f;
  s_specRunning = false;
#endif
  
  LINE_LOG("[LINE] radio path restored, line path resources released "
           "(XSMT stays low until the player unmutes)\n");
}

// Невдача під час старту ПІСЛЯ прийнятого запиту: записати причину й відкотитися на радіо.
void abortStart(const char* why, LinePathError err) {
  s_lastError = err;
  LINE_LOG("[LINE] start failed: %s - rolling back to radio path\n", why);
  doRadio();
}

void doStart() {
  muteDac(true);
  s_dsp.forceMute();
  s_recovering = false;

  // 1. Радіо: зупинити й дати дограти хвіст буфера.
  AudioPlayer::stop();
  const uint32_t w0 = millis();
  while (AudioPlayer::state() != PlayerState::Idle) {
    if (millis() - w0 > cfg::kRadioStopWaitMs) {
      char why[64];
      snprintf(why, sizeof(why), "radio did not reach Idle (state %s)",
               AudioPlayer::stateName(AudioPlayer::state()));
      abortStart(why, LinePathError::RadioStopTimeout);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  vTaskDelay(pdMS_TO_TICKS(cfg::kRadioDrainMs));

  // 2. Зафіксувати 48 кГц на I2S0.
  if (!AudioPlayer::setFixedOutputRate(true)) {
    abortStart("player refused fixed output rate", LinePathError::RadioRefused);
    return;
  }
  const uint32_t c0 = millis();
  while (!AudioPlayer::fixedOutputRate()) {
    if (millis() - c0 > cfg::kClockApplyWaitMs) {
      abortStart("48 kHz not applied on I2S0", LinePathError::ClockFixFailed);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  // 3. DOUT -> I2S1 TX (під мʼютом).
  routeDout(true);

  // 4. Канали: RX, потім TX.
  esp_err_t e = i2s_channel_enable(s_rx);
  if (e != ESP_OK) {
    char why[64];
    snprintf(why, sizeof(why), "RX enable: %s", esp_err_to_name(e));
    abortStart(why, LinePathError::EnableFailed);
    return;
  }
  s_rxOn = true;

  e = i2s_channel_enable(s_tx);
  if (e != ESP_OK) {
    char why[64];
    snprintf(why, sizeof(why), "TX enable: %s", esp_err_to_name(e));
    disableChannels();
    abortStart(why, LinePathError::EnableFailed);
    return;
  }
  s_txOn = true;

  s_pinsOk = restorePinOutputs();
  resetDsp();
  s_settle = cfg::kSettleBlocks;
  s_lastGoodMs = millis();
  s_state = LinePathState::Starting;
  applyMute();
  
#if LINE_PATH_DIAG
  LINE_LOG("[LP] cfg: I2S1 SLAVE 32-bit Philips, MCLK from I2S0 (48kHz fixed). Use 'lp.cfg' for stats.\n");
#endif
}

// Тіло задачі. Перша дія — створення каналів і сигнал результату; завершується після doRadio().
void taskEntry(void*) {
  const esp_err_t e = createChannels();
  if (e != ESP_OK || s_initAbandoned) {
    const bool abandoned = s_initAbandoned;
    if (e == ESP_OK) {
      destroyChannels();
    } else {
      s_lastError = LinePathError::ChannelCreateFailed;
    }
    freeBuffer();
    s_req = kReqNone;
    s_initErr = static_cast<int>(e);
    s_stackFreeMin = uxTaskGetStackHighWaterMark(nullptr);
    s_task = nullptr;
    s_taskAlive = false;
    if (!abandoned) {
      xSemaphoreGive(s_initSem);
    }
    vTaskDelete(nullptr);
    return;
  }

  s_initErr = 0;
  s_heapFreeAfter = static_cast<uint32_t>(heapFree());
  s_heapLargestAfter = static_cast<uint32_t>(heapLargest());
  LINE_LOG("[LINE] resources up: used %d B internal RAM (free %u -> %u B, largest %u -> %u B)\n",
           static_cast<int>(s_heapFreeBefore) - static_cast<int>(s_heapFreeAfter),
           static_cast<unsigned>(s_heapFreeBefore), static_cast<unsigned>(s_heapFreeAfter),
           static_cast<unsigned>(s_heapLargestBefore), static_cast<unsigned>(s_heapLargestAfter));
  xSemaphoreGive(s_initSem);

  for (;;) {
    const uint8_t r = s_req;
    s_req = kReqNone;
    if (r == kReqStart) {
      if (s_state == LinePathState::Off) doStart();
    } else if (r == kReqStop) {
      doStop();
    } else if (r == kReqRadio) {
      doRadio();
    }
    if (s_exit) {
      s_exit = false;
      s_stackFreeMin = uxTaskGetStackHighWaterMark(nullptr);
      s_task = nullptr;
      s_taskAlive = false;
      vTaskDelete(nullptr);
      return;
    }
    if (s_state == LinePathState::Off) {
      vTaskDelay(pdMS_TO_TICKS(cfg::kIdlePollMs));
    } else {
      pump();
    }
  }
}

// Спільна частина start() і requestStart(). wait = true: чекає створення каналів (до kCreateWaitMs).
bool startLocked(bool wait) {
  if (s_req != kReqNone) {
    return false;
  }
  s_lastError = LinePathError::None;  // нова спроба скидає попередню причину

  if (s_taskAlive) {
    if (s_state != LinePathState::Off) {
      return false;
    }
    s_startReqMs = millis();
    muteDac(true);
    s_req = kReqStart;
    return true;
  }

  if (s_state != LinePathState::Off) {
    return false;
  }
  s_startReqMs = millis();
  muteDac(true);

  s_heapFreeBefore = static_cast<uint32_t>(heapFree());
  s_heapLargestBefore = static_cast<uint32_t>(heapLargest());

  s_buf = static_cast<int32_t*>(heap_caps_malloc(kAllocBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (s_buf == nullptr) {
    s_allocFailures = s_allocFailures + 1;
    s_lastError = LinePathError::AllocFailed;
    LINE_LOG("[LINE] start rejected: no memory for block buffer (%u B); free=%u B, largest=%u B\n",
             static_cast<unsigned>(kAllocBytes), static_cast<unsigned>(heapFree()),
             static_cast<unsigned>(heapLargest()));
    return false;
  }
  s_fbuf = reinterpret_cast<float*>(s_buf + cfg::kBlockFrames * 2);  // друга половина виділення

  s_initAbandoned = false;
  s_initErr = 0;
  s_exit = false;
  xSemaphoreTake(s_initSem, 0);  // скинути можливий застарілий сигнал
  s_req = kReqStart;
  s_taskAlive = true;

  const BaseType_t ok = xTaskCreatePinnedToCore(taskEntry, "line_path", cfg::kTaskStackBytes, nullptr,
                                                 cfg::kTaskPriority, &s_task, cfg::kTaskCore);
  if (ok != pdPASS) {
    s_task = nullptr;
    s_taskAlive = false;
    s_req = kReqNone;
    freeBuffer();
    s_allocFailures = s_allocFailures + 1;
    s_lastError = LinePathError::TaskCreateFailed;
    LINE_LOG("[LINE] start rejected: task create failed (stack %u B); free=%u B, largest=%u B\n",
             static_cast<unsigned>(cfg::kTaskStackBytes), static_cast<unsigned>(heapFree()),
             static_cast<unsigned>(heapLargest()));
    return false;
  }

  if (!wait) {
    LINE_LOG("[LINE] start accepted (channels are created in the task)\n");
    return true;
  }

  if (xSemaphoreTake(s_initSem, pdMS_TO_TICKS(cfg::kCreateWaitMs)) != pdTRUE &&
      xSemaphoreTake(s_initSem, 0) != pdTRUE) {
    s_initAbandoned = true;  // задача сама згорнеться, коли дійде до цього місця
    s_allocFailures = s_allocFailures + 1;
    s_lastError = LinePathError::CreateTimeout;
    LINE_LOG("[LINE] start rejected: resource creation timed out after %lu ms\n",
             static_cast<unsigned long>(cfg::kCreateWaitMs));
    return false;
  }

  if (s_initErr != 0) {
    s_allocFailures = s_allocFailures + 1;
    if (s_lastError == LinePathError::None) {
      s_lastError = LinePathError::ChannelCreateFailed;
    }
    LINE_LOG("[LINE] start rejected: I2S1 channels not created (%s); XSMT low, DOUT stays on I2S0\n",
             esp_err_to_name(static_cast<esp_err_t>(s_initErr)));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Доступ для Serial-тесту
// ---------------------------------------------------------------------------

[[maybe_unused]] DbgInfo dbgInfo() {
  DbgInfo d = {};
  d.alive = s_taskAlive;
  const TaskHandle_t t = s_task;
  d.stackFreeMin = (d.alive && t != nullptr) ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(t))
                                              : s_stackFreeMin;
  d.heapFreeBefore = s_heapFreeBefore;
  d.heapLargestBefore = s_heapLargestBefore;
  d.heapFreeAfter = s_heapFreeAfter;
  d.heapLargestAfter = s_heapLargestAfter;
  d.recoveries = s_recoveries;
#if ENABLE_VU
  d.vuPeakLeftDbfs = (s_vuMaxL > 0) ? 20.0f * log10f(static_cast<float>(s_vuMaxL) / vu_cfg::kFullScale) : -99.0f;
  d.vuPeakRightDbfs = (s_vuMaxR > 0) ? 20.0f * log10f(static_cast<float>(s_vuMaxR) / vu_cfg::kFullScale) : -99.0f;
#else
  d.vuPeakLeftDbfs = -99.0f;
  d.vuPeakRightDbfs = -99.0f;
#endif
  return d;
}

[[maybe_unused]] bool dbgFault(uint32_t ms) {
  if (s_state != LinePathState::Running || s_faultUntilMs != 0) {
    return false;
  }
  s_faultRx0 = s_rxOvf;
  s_faultTx0 = s_txOvf;
  uint32_t until = millis() + ms;
  if (until == 0) until = 1;
  s_faultUntilMs = until;
  return true;
}

}  // namespace

#else  // LINE_PATH_ENABLE

namespace {
[[maybe_unused]] DbgInfo dbgInfo() { return DbgInfo{}; }
[[maybe_unused]] bool dbgFault(uint32_t) { return false; }
}  // namespace

#endif  // LINE_PATH_ENABLE

// ===========================================================================
// Serial-тест (працює і при LINE_PATH_ENABLE == 0: тоді лише lp.heap)
// ===========================================================================

namespace {

#if LINE_PATH_TEST

#define LP_UL(x) static_cast<unsigned long>(x)

void printHeap() {
  lineOut("[LINE] heap internal: free=%u B, largest block=%u B, min free ever=%u B "
          "(LINE_PATH_ENABLE=%d)\n",
          static_cast<unsigned>(heapFree()), static_cast<unsigned>(heapLargest()),
          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)), LINE_PATH_ENABLE);
}

#if LINE_PATH_ENABLE

// lp.stat: кожен рядок — окремий lineOut (повний, з \n).
void printStats() {
  const LinePathStats s = LinePath::stats();
  const DbgInfo       d = dbgInfo();
  int8_t              g[eq_cfg::kBandCount];
  s_eqLine.getAllDb(g);

  lineOut("[LINE] state=%s xsmt=%s userMute=%d dout=%s volume=%.2f dB resources=%s\n",
          LinePath::stateName(s.state), s.muted ? "low" : "high", s.userMuted ? 1 : 0,
          s.routedToLine ? "I2S1(line)" : "I2S0(radio)", static_cast<double>(s.volumeDb),
          d.alive ? "up" : "down");
  lineOut("[LINE] blocks=%lu starts=%lu stalls=%lu recoveries=%lu readTimeouts=%lu readErrors=%lu\n",
          LP_UL(s.blocks), LP_UL(s.starts), LP_UL(s.stalls), LP_UL(d.recoveries), LP_UL(s.readTimeouts),
          LP_UL(s.readErrors));
  lineOut("[LINE] writeTimeouts=%lu rxOvf=%lu txOvf=%lu allocFailures=%lu maxProcess=%lu us "
          "maxReadWait=%lu us\n",
          LP_UL(s.writeTimeouts), LP_UL(s.rxOverflows), LP_UL(s.txOverflows), LP_UL(s.allocFailures),
          LP_UL(s.maxProcessUs), LP_UL(s.maxReadWaitUs));
  lineOut("[LINE] peaks since lp.reset: in L %.1f R %.1f dBFS | out L %.1f R %.1f dBFS\n",
          static_cast<double>(s.peakLeftDbfs), static_cast<double>(s.peakRightDbfs),
          static_cast<double>(s.peakOutLeftDbfs), static_cast<double>(s.peakOutRightDbfs));
  lineOut("[LINE] vu peak since lp.reset (screen scale, offset %+.0f dB, after EQ, before volume): "
          "L %.1f R %.1f dBFS\n",
          static_cast<double>(cfg::kVuOffsetDb), static_cast<double>(d.vuPeakLeftDbfs),
          static_cast<double>(d.vuPeakRightDbfs));
  lineOut("[LINE] eq: clipCount=%lu (final saturation after volume) bypass=%d gains=%d %d %d %d %d dB\n",
          LP_UL(s.clipCount),
          s_eqLine.bypass() ? 1 : 0, static_cast<int>(g[0]), static_cast<int>(g[1]), static_cast<int>(g[2]),
          static_cast<int>(g[3]), static_cast<int>(g[4]));
  lineOut("[LINE] initMs=%lu lastStartMs=%lu stackFreeMin=%u B%s lastError=%s\n", LP_UL(s.initMs),
          LP_UL(s.lastStartMs), static_cast<unsigned>(d.stackFreeMin), d.alive ? "" : " (last run)",
          LinePath::errorName(s.lastError));
  lineOut("[LINE] last create: free %u -> %u B, largest %u -> %u B\n", static_cast<unsigned>(d.heapFreeBefore),
          static_cast<unsigned>(d.heapFreeAfter), static_cast<unsigned>(d.heapLargestBefore),
          static_cast<unsigned>(d.heapLargestAfter));
  printHeap();
}

// lp.fault [мс]: зупинити споживання RX і простежити Running -> Stalled -> Starting -> Running.
void cmdFault(uint32_t ms) {
  if (ms <= cfg::kStallMs || ms > cfg::kFaultMaxMs) {
    lineOut("[LINE] fault: duration must be > kStallMs (%lu) and <= %lu ms\n",
            LP_UL(cfg::kStallMs), LP_UL(cfg::kFaultMaxMs));
    return;
  }
  if (!dbgFault(ms)) {
    lineOut("[LINE] fault rejected: state must be Running with no fault active (lp.start first)\n");
    return;
  }
  lineOut("[LINE] fault: RX blocks not consumed for %lu ms (stall threshold %lu ms)\n", LP_UL(ms),
          LP_UL(cfg::kStallMs));

  const uint32_t t0 = millis();
  const uint32_t deadline = ms + cfg::kFaultRecoverWaitMs;
  LinePathState  last = LinePathState::Running;
  bool           sawStalled = false;
  bool           mutedInStall = false;
  bool           recovered = false;

  while (millis() - t0 < deadline) {
    const LinePathStats s = LinePath::stats();
    if (s.state != last) {
      lineOut("[LINE] fault t=%lu ms: %s -> %s (xsmt muted=%d)\n", LP_UL(millis() - t0),
              LinePath::stateName(last), LinePath::stateName(s.state), s.muted ? 1 : 0);
      last = s.state;
    }
    if (s.state == LinePathState::Stalled) {
      sawStalled = true;
      if (s.muted) mutedInStall = true;
    }
    if (sawStalled && s.state == LinePathState::Running && !s.muted) {
      recovered = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  const LinePathStats f = LinePath::stats();
  lineOut("[LINE] fault result: stalled=%s mutedInStall=%s recovered=%s -> %s (stalls=%lu, "
          "rxOvf=%lu txOvf=%lu; overflows during the injected fault are expected)\n",
          sawStalled ? "yes" : "NO", mutedInStall ? "yes" : "NO", recovered ? "yes" : "NO",
          (sawStalled && mutedInStall && recovered) ? "PASS" : "FAIL", LP_UL(f.stalls), LP_UL(f.rxOverflows),
          LP_UL(f.txOverflows));
}

bool waitState(LinePathState want, uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (LinePath::state() != want) {
    if (millis() - t0 > timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

bool waitTaskGone(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (dbgInfo().alive) {
    if (millis() - t0 > timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

// lp.cycle <N>: N разів start -> пауза -> returnToRadio, друк free / largest після кожного.
void cmdCycle(int n) {
  if (n < 1 || n > static_cast<int>(cfg::kCycleMaxCount)) {
    lineOut("[LINE] cycle: N must be 1..%lu\n", LP_UL(cfg::kCycleMaxCount));
    return;
  }
  if (LinePath::state() != LinePathState::Off || dbgInfo().alive) {
    lineOut("[LINE] cycle rejected: line path must be Off with resources released (lp.radio first)\n");
    return;
  }
  if (AudioPlayer::state() != PlayerState::Idle) {
    lineOut("[LINE] cycle rejected: radio is not Idle (switch to an external input / stop it first)\n");
    return;
  }

  const LinePathStats s0 = LinePath::stats();
  const int           baseFree = static_cast<int>(heapFree());
  const int           baseLargest = static_cast<int>(heapLargest());
  uint32_t            prevRx = s0.rxOverflows;
  uint32_t            prevTx = s0.txOverflows;
  int                 minLargest = baseLargest;
  int                 lastFree = baseFree;
  int                 lastLargest = baseLargest;
  uint32_t            sumRx = 0;
  uint32_t            sumTx = 0;
  int                 done = 0;

  lineOut("[LINE] cycle x%d: baseline (Idle, resources down) free=%d B, largest=%d B, rxOvf=%lu, txOvf=%lu\n",
          n, baseFree, baseLargest, LP_UL(prevRx), LP_UL(prevTx));

  for (int i = 1; i <= n; ++i) {
    if (!LinePath::start()) {
      lineOut("[LINE] cycle %d: start() rejected (%s) - abort\n", i,
              LinePath::errorName(LinePath::lastError()));
      break;
    }
    if (!waitState(LinePathState::Running, cfg::kCycleStartWaitMs)) {
      lineOut("[LINE] cycle %d: not Running within %lu ms (state %s) - abort\n", i,
              LP_UL(cfg::kCycleStartWaitMs), LinePath::stateName(LinePath::state()));
      LinePath::returnToRadio();
      waitTaskGone(cfg::kCycleRadioWaitMs);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::kCycleRunMs));
    const int upFree = static_cast<int>(heapFree());
    const int upLargest = static_cast<int>(heapLargest());
    LinePath::returnToRadio();
    const bool gone = waitTaskGone(cfg::kCycleRadioWaitMs);
    vTaskDelay(pdMS_TO_TICKS(cfg::kCycleFreeSettleMs));
    const int           f = static_cast<int>(heapFree());
    const int           l = static_cast<int>(heapLargest());
    const LinePathStats s = LinePath::stats();
    const uint32_t      dRx = s.rxOverflows - prevRx;
    const uint32_t      dTx = s.txOverflows - prevTx;
    prevRx = s.rxOverflows;
    prevTx = s.txOverflows;
    sumRx += dRx;
    sumTx += dTx;
    if (l < minLargest) minLargest = l;
    lastFree = f;
    lastLargest = l;
    ++done;
    lineOut("[LINE] cycle %d/%d: up free=%d largest=%d | down free=%d largest=%d "
            "(vs baseline: free %+d, largest %+d) | rx+%lu tx+%lu stalls=%lu%s\n",
            i, n, upFree, upLargest, f, l, f - baseFree, l - baseLargest, LP_UL(dRx), LP_UL(dTx),
            LP_UL(s.stalls), gone ? "" : " WARNING: task still alive");
    if (!gone) break;
  }
  lineOut("[LINE] cycle summary: %d done; free %d -> %d B (%+d), largest %d -> %d B (%+d), min largest %d B; "
          "overflows on transitions: rx +%lu, tx +%lu; allocFailures=%lu\n",
          done, baseFree, lastFree, lastFree - baseFree, baseLargest, lastLargest, lastLargest - baseLargest,
          minLargest, LP_UL(sumRx), LP_UL(sumTx), LP_UL(LinePath::stats().allocFailures));
}

// lp.vu: 3 с друкує рівні VU (0..1 за шкалою екрана) раз на kVuTestPrintMs. Читає ті самі атомарні піки, що й
// екран (exchange), тож на цей час дає йому частину пікiв: лише для налагодження.
void cmdVu() {
  static VuSourceLinePath src;
  lineOut("[LINE] vu: %lu ms, norm 0..1 (0 dBFS = 1.0, floor %.0f dB); the screen meter may flicker meanwhile\n",
          LP_UL(cfg::kVuTestMs), static_cast<double>(vu_cfg::kDbFloor));
  const uint32_t t0 = millis();
  uint32_t       lastPrint = t0;
  float          maxL = 0.0f;
  float          maxR = 0.0f;
  bool           anyData = false;

  while (millis() - t0 < cfg::kVuTestMs) {
    float      l = 0.0f;
    float      r = 0.0f;
    const bool ok = src.read(l, r);
    if (ok) {
      anyData = true;
      if (l > maxL) maxL = l;
      if (r > maxR) maxR = r;
    }
    if (millis() - lastPrint >= cfg::kVuTestPrintMs) {
      lastPrint = millis();
      lineOut("[LINE] vu: %s L %.2f (%.1f dB) R %.2f (%.1f dB)\n", ok ? "data" : "no data",
              static_cast<double>(maxL), static_cast<double>(vu_cfg::kDbFloor * (1.0f - maxL)),
              static_cast<double>(maxR), static_cast<double>(vu_cfg::kDbFloor * (1.0f - maxR)));
      maxL = 0.0f;
      maxR = 0.0f;
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::kVuTestPollMs));
  }
  if (!anyData) {
    lineOut("[LINE] vu: no data (line path not Running, ENABLE_VU = 0, or no RX blocks)\n");
  }
}

// lp.api: самотест публічного API. Не запускає прохід. Значення гучності / EQ тимчасово змінюються
// і відновлюються; якщо прохід працює, на цей час можлива коротка зміна звуку.
void apiCheck(const char* name, bool ok, int& fails) {
  lineOut("[LINE] api %-30s %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) {
    ++fails;
  }
}

void cmdApi() {
  const float oldVol = LinePath::volumeDb();
  const bool  oldMute = LinePath::userMuted();
  int8_t      oldEq[eq_cfg::kBandCount];
  LinePath::getEqAllDb(oldEq);
  const bool oldBypass = LinePath::eqBypass();
  int        fails = 0;

  lineOut("[LINE] api self-test start\n");

  LinePath::setVolumeDb(-40.0f);
  apiCheck("volume set -40", fabsf(LinePath::volumeDb() + 40.0f) < 0.01f, fails);
  LinePath::setVolumeDb(-200.0f);
  apiCheck("volume clamp low -120", fabsf(LinePath::volumeDb() - line_path_cfg::kVolMinDb) < 0.01f, fails);
  LinePath::setVolumeDb(0.0f);
  apiCheck("volume clamp high -6", fabsf(LinePath::volumeDb() - line_path_cfg::kVolMaxDb) < 0.01f, fails);
  LinePath::setVolumeDb(-37.5f);
  apiCheck("volume fractional -37.5", fabsf(LinePath::volumeDb() + 37.5f) < 0.01f, fails);

  LinePath::setAttenuationDb(-100);
  apiCheck("att wrapper clamp -60", LinePath::attenuationDb() == line_path_cfg::kAttMinDb, fails);
  apiCheck("stats volume matches", fabsf(LinePath::stats().volumeDb - LinePath::volumeDb()) < 0.01f, fails);

  LinePath::setUserMute(true);
  apiCheck("user mute on", LinePath::userMuted(), fails);
  LinePath::setUserMute(false);
  apiCheck("user mute off", !LinePath::userMuted(), fails);

  LinePath::setEqBandDb(2, 3);
  apiCheck("eq band set 3 dB", LinePath::getEqBandDb(2) == 3, fails);
  LinePath::setEqBandDb(2, 50);
  apiCheck("eq band clamp +12", LinePath::getEqBandDb(2) == eq_cfg::kGainMaxDb, fails);
  LinePath::setEqBandDb(9, 5);
  apiCheck("eq bad band ignored", LinePath::getEqBandDb(9) == 0, fails);
  const int8_t pat[eq_cfg::kBandCount] = {1, -2, 3, -4, 5};
  LinePath::setEqAllDb(pat);
  int8_t got[eq_cfg::kBandCount] = {};
  LinePath::getEqAllDb(got);
  bool allEq = true;
  for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
    allEq = allEq && (got[b] == pat[b]);
  }
  apiCheck("eq all set/get", allEq, fails);

  LinePath::setEqBypass(true);
  apiCheck("eq bypass on", LinePath::eqBypass(), fails);
  LinePath::setEqBypass(false);
  apiCheck("eq bypass off", !LinePath::eqBypass(), fails);

  // Відновлення попереднього стану.
  LinePath::setVolumeDb(oldVol);
  LinePath::setUserMute(oldMute);
  LinePath::setEqAllDb(oldEq);
  LinePath::setEqBypass(oldBypass);

  lineOut("[LINE] api self-test: %s (%d failed)\n", fails == 0 ? "PASS" : "FAIL", fails);
}

#undef LP_UL

#endif  // LINE_PATH_ENABLE

void handleLine(const char* line) {
  if (strncmp(line, "lp.", 3) != 0) {
    return;
  }
  const char* c = line + 3;
  if (strcmp(c, "heap") == 0) {
    printHeap();
    return;
  }
#if LINE_PATH_ENABLE
  if (strcmp(c, "start") == 0) {
    // Тестовий режим: звук одразу. Мʼют користувача знімається ДО старту (документовано).
    LinePath::setUserMute(false);
    lineOut(LinePath::start() ? "[LINE] start requested (user mute off)\n"
                               : "[LINE] start rejected\n");
  } else if (strcmp(c, "rstart") == 0) {
    LinePath::setUserMute(false);
    lineOut(LinePath::requestStart() ? "[LINE] requestStart accepted (user mute off)\n"
                                      : "[LINE] requestStart rejected\n");
  } else if (strcmp(c, "stop") == 0) {
    lineOut(LinePath::stop() ? "[LINE] stop requested\n" : "[LINE] stop rejected\n");
  } else if (strcmp(c, "radio") == 0) {
    lineOut(LinePath::returnToRadio() ? "[LINE] radio requested\n" : "[LINE] radio rejected\n");
  } else if (strcmp(c, "stat") == 0) {
    printStats();
#if LINE_PATH_DIAG
  } else if (strcmp(c, "cfg") == 0) {
    lineOut("[LP] cfg: I2S0 (master) fs=48000 (fixed), MCLK=GPIO16, BCLK=GPIO15, WS=GPIO17\n");
    lineOut("[LP] cfg: I2S1 (slave) role=SLAVE, fmt=PHILIPS, data=32bit, slot=32bit, DMA=%lux%lu\n",
            (unsigned long)cfg::kDmaDescCount, (unsigned long)cfg::kDmaFrameCount);
    lineOut("[LP] cfg: RX->Float formula: int32 * 2^-31 (assumes 24-bit MSB left-aligned in 32-bit slot)\n");
    
    uint32_t rx0 = s_diagRxFrames;
    uint32_t tx0 = s_diagTxFrames;
    uint32_t t0 = millis();
    vTaskDelay(pdMS_TO_TICKS(2000));
    uint32_t rx1 = s_diagRxFrames;
    uint32_t tx1 = s_diagTxFrames;
    uint32_t t1 = millis();
    float dt = (t1 - t0) / 1000.0f;
    lineOut("[LP] cfg: measured over %.1fs: RX=%.0f fps, TX=%.0f fps (expect 48000)\n", 
            dt, (rx1 - rx0) / dt, (tx1 - tx0) / dt);
    lineOut("[LP] cfg: stats: rxOvf=%lu, txOvf=%lu, stalls=%lu\n", 
            (unsigned long)s_rxOvf, (unsigned long)s_txOvf, (unsigned long)s_stalls);
            
  } else if (strncmp(c, "raw", 3) == 0) {
    int n = 16;
    if (c[3] == ' ') n = atoi(c + 4);
    if (n < 1) n = 1;
    if (n > 128) n = 128;
    
    lineOut("[LP] raw: printing %d raw RX samples (int32)\n", n);
    int32_t tmp[256]; // 128 stereo frames
    size_t got = 0;
    // Тимчасово зупиняємо pump, щоб прочитати сирий блок
    // Увага: це може викликати underrun TX, але для діагностики допустимо
    i2s_channel_read(s_rx, tmp, n * 2 * sizeof(int32_t), &got, 100);
    int frames = got / (2 * sizeof(int32_t));
    int nonzeroLower8 = 0;
    for (int i = 0; i < frames; ++i) {
      lineOut("  [%02d] L: 0x%08lX (%ld) | R: 0x%08lX (%ld)\n", 
              i, (unsigned long)tmp[i*2], (long)tmp[i*2], 
              (unsigned long)tmp[i*2+1], (long)tmp[i*2+1]);
      if ((tmp[i*2] & 0xFF) != 0 || (tmp[i*2+1] & 0xFF) != 0) nonzeroLower8++;
    }
    lineOut("[LP] raw: frames read=%d, samples with non-zero lower 8 bits=%d (expect 0 if 24-bit left-aligned)\n", 
            frames, nonzeroLower8 * 2);
            
  } else if (strncmp(c, "spec", 4) == 0) {
    int sec = 2;
    if (c[4] == ' ') sec = atoi(c + 5);
    if (sec < 1) sec = 1;
    if (sec > 10) sec = 10;
    
    lineOut("[LP] spec: measuring RAW RX spectrum for %d sec (Goertzel)...\n", sec);
    for (size_t k = 0; k < line_path_diag_cfg::kGoertzelCount; ++k) {
      goertzelInit(s_specL[k], line_path_diag_cfg::kGoertzelFreqs[k], cfg::kFsHz);
      goertzelInit(s_specR[k], line_path_diag_cfg::kGoertzelFreqs[k], cfg::kFsHz);
    }
    s_specBlocks = 0;
    s_specRunning = true;
    s_specEndMs = millis() + sec * 1000;
    
    uint32_t maxProcStart = s_maxProcessUs;
    while (millis() < s_specEndMs) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_specRunning = false;
    uint32_t maxProcEnd = s_maxProcessUs;
    
    lineOut("[LP] spec: blocks processed=%lu, maxProcess delta=%lu us\n", 
            (unsigned long)s_specBlocks, (unsigned long)(maxProcEnd - maxProcStart));
    lineOut("[LP] spec: Freq(Hz) | L(dBFS) | R(dBFS)\n");
    lineOut("-------------------------------------\n");
    for (size_t k = 0; k < line_path_diag_cfg::kGoertzelCount; ++k) {
      float dbL = goertzelMagnitudeDb(s_specL[k]);
      float dbR = goertzelMagnitudeDb(s_specR[k]);
      lineOut("  %5.0f    | %+6.1f  | %+6.1f\n", line_path_diag_cfg::kGoertzelFreqs[k], dbL, dbR);
    }
    
  } else if (strncmp(c, "bypass", 6) == 0) {
    int v = 0;
    if (c[6] == ' ') v = atoi(c + 7);
    LinePath::diagSetBypass(v != 0);
    lineOut("[LP] bypass = %d (DSP %s)\n", v != 0 ? 1 : 0, v ? "BYPASSED" : "ACTIVE");
    
  } else if (strncmp(c, "tone", 4) == 0) {
    if (strcmp(c + 4, " off") == 0 || strcmp(c + 4, " 0") == 0) {
      LinePath::diagSetTone(0.0f);
      lineOut("[LP] tone OFF\n");
    } else {
      float hz = atof(c + 5);
      if (hz < 20.0f || hz > 20000.0f) {
        lineOut("[LP] tone: freq must be 20..20000 Hz\n");
      } else {
        LinePath::diagSetTone(hz, -12.0f);
        lineOut("[LP] tone = %.0f Hz @ -12 dBFS (replaces RX data)\n", hz);
      }
    }
#endif
  } else if (strncmp(c, "att ", 4) == 0) {
    const int v = atoi(c + 4);
    if (v < line_path_cfg::kAttMinDb || v > line_path_cfg::kAttMaxDb) {
      lineOut("[LINE] att out of range (%d..%d dB); use lp.vol for -120..-6\n",
              static_cast<int>(line_path_cfg::kAttMinDb), static_cast<int>(line_path_cfg::kAttMaxDb));
    } else {
      LinePath::setAttenuationDb(static_cast<int8_t>(v));
      lineOut("[LINE] att = %d dB (volume)\n", v);
    }
  } else if (strncmp(c, "vol ", 4) == 0) {
    const float v = static_cast<float>(atof(c + 4));
    if (!(v >= line_path_cfg::kVolMinDb && v <= line_path_cfg::kVolMaxDb)) {
      lineOut("[LINE] vol out of range (%.0f..%.0f dB)\n", static_cast<double>(line_path_cfg::kVolMinDb),
              static_cast<double>(line_path_cfg::kVolMaxDb));
    } else {
      if (v < line_path_cfg::kWarnVolDb) {
        lineOut("[LINE] WARNING: %.2f dB is below the usual %.0f..%.0f dB test range\n",
                static_cast<double>(v), static_cast<double>(line_path_cfg::kWarnVolDb),
                static_cast<double>(line_path_cfg::kVolMaxDb));
      }
      LinePath::setVolumeDb(v);
      lineOut("[LINE] volume = %.2f dB\n", static_cast<double>(v));
    }
  } else if (strncmp(c, "mute ", 5) == 0 || strncmp(c, "usermute ", 9) == 0) {
    const int v = atoi(c + (c[0] == 'm' ? 5 : 9));
    LinePath::setUserMute(v != 0);
    lineOut("[LINE] user mute = %d\n", v != 0 ? 1 : 0);
  } else if (strncmp(c, "eq ", 3) == 0) {
    int band = -1;
    int db = 0;
    if (sscanf(c + 3, "%d %d", &band, &db) != 2 || band < 0 || band >= eq_cfg::kBandCount ||
        db < eq_cfg::kGainMinDb || db > eq_cfg::kGainMaxDb) {
      lineOut("[LINE] eq: usage lp.eq <band 0..%d> <%d..%d dB>\n",
              static_cast<int>(eq_cfg::kBandCount) - 1, eq_cfg::kGainMinDb, eq_cfg::kGainMaxDb);
    } else {
      LinePath::setEqBandDb(static_cast<uint8_t>(band), static_cast<int8_t>(db));
      lineOut("[LINE] eq band %d = %d dB\n", band, db);
    }
  } else if (strncmp(c, "eqall ", 6) == 0) {
    int g[eq_cfg::kBandCount] = {};
    const int cnt = sscanf(c + 6, "%d %d %d %d %d", &g[0], &g[1], &g[2], &g[3], &g[4]);
    bool ok = (cnt == static_cast<int>(eq_cfg::kBandCount));
    for (uint8_t b = 0; ok && b < eq_cfg::kBandCount; ++b) {
      ok = (g[b] >= eq_cfg::kGainMinDb && g[b] <= eq_cfg::kGainMaxDb);
    }
    if (!ok) {
      lineOut("[LINE] eqall: usage lp.eqall <5 values, %d..%d dB each>\n", eq_cfg::kGainMinDb,
              eq_cfg::kGainMaxDb);
    } else {
      int8_t v[eq_cfg::kBandCount];
      for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) {
        v[b] = static_cast<int8_t>(g[b]);
      }
      LinePath::setEqAllDb(v);
      lineOut("[LINE] eqall = %d %d %d %d %d dB\n", g[0], g[1], g[2], g[3], g[4]);
    }
  } else if (strncmp(c, "eqbypass ", 9) == 0) {
    const int v = atoi(c + 9);
    LinePath::setEqBypass(v != 0);
    lineOut("[LINE] eq bypass = %d\n", v != 0 ? 1 : 0);
  } else if (strcmp(c, "eqshow") == 0) {
    int8_t g[eq_cfg::kBandCount];
    LinePath::getEqAllDb(g);
    lineOut("[LINE] eq gains %d %d %d %d %d dB, bypass=%d, clipCount=%lu\n", static_cast<int>(g[0]),
            static_cast<int>(g[1]), static_cast<int>(g[2]), static_cast<int>(g[3]), static_cast<int>(g[4]),
            LinePath::eqBypass() ? 1 : 0, static_cast<unsigned long>(LinePath::stats().clipCount));
  } else if (strcmp(c, "reset") == 0) {
    LinePath::resetStats();
    lineOut("[LINE] stats reset\n");
  } else if (strcmp(c, "fault") == 0) {
    cmdFault(cfg::kFaultDefaultMs);
  } else if (strncmp(c, "fault ", 6) == 0) {
    const int v = atoi(c + 6);
    cmdFault(v > 0 ? static_cast<uint32_t>(v) : 0u);
  } else if (strncmp(c, "cycle ", 6) == 0) {
    cmdCycle(atoi(c + 6));
  } else if (strcmp(c, "api") == 0) {
    cmdApi();
  } else if (strcmp(c, "vu") == 0) {
    cmdVu();
  } else {
    lineOut("[LINE] commands: lp.start lp.rstart lp.stop lp.radio lp.stat lp.reset\n");
    lineOut("[LINE] lp.att <dB> lp.vol <dB> lp.mute|lp.usermute 0|1 lp.eq <band> <dB> lp.eqall <5 values>\n");
#if LINE_PATH_DIAG
    lineOut("[LINE] diag:   lp.cfg lp.raw [n] lp.spec [sec] lp.bypass <0|1> lp.tone <hz|off>\n");
#endif
    lineOut("[LINE] lp.eqbypass 0|1 lp.eqshow lp.vu lp.fault [ms] lp.cycle <N> lp.api lp.heap\n");
  }
#else
  lineOut("[LINE] line path disabled (LINE_PATH_ENABLE = 0): only lp.heap is available\n");
#endif
}

void testTask(void*) {
  char   line[cfg::kTestLineMax];
  size_t n = 0;
  for (;;) {
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
    vTaskDelay(pdMS_TO_TICKS(cfg::kTestPollMs));
  }
}

void startTestTask() {
  static bool started = false;
  if (started) {
    return;
  }
  TaskHandle_t testHandle = nullptr;
  if (xTaskCreatePinnedToCore(testTask, "line_test", cfg::kTestTaskStackBytes, nullptr,
                               cfg::kTestTaskPriority, &testHandle, cfg::kTestTaskCore) == pdPASS) {
    started = true;
    lineOut("[LINE] test mode ready (send 'lp.help' or 'lp.api')\n");
  } else {
    lineOut("[LINE] test task create failed\n");
  }
}

#else  // LINE_PATH_TEST

[[maybe_unused]] void startTestTask() {}

#endif  // LINE_PATH_TEST

}  // namespace

// ===========================================================================
// Публічний інтерфейс
// ===========================================================================

#if LINE_PATH_ENABLE

bool LinePath::begin() {
  if (s_ready) {
    return true;
  }
  const uint32_t t0 = millis();
  s_initSem = xSemaphoreCreateBinaryStatic(&s_initSemBuf);
  if (s_initSem == nullptr) {
    lineOut("[LINE] begin failed: semaphore\n");
    return false;
  }
  s_eqLine.setSampleRate(cfg::kFsHz);
  s_dsp.setSampleRate(cfg::kFsHz);
  s_dsp.setAttenuationDb(cfg::kStartVolumeDb);
  s_dsp.forceMute();
#if ENABLE_VU
  s_vuScale = vu_cfg::kFullScale * powf(10.0f, cfg::kVuOffsetDb / 20.0f);
#endif
  applyMute();  // мʼют користувача типово увімкнений; стан Off
  startTestTask();
  s_initMs = millis() - t0;
  s_ready = true;
  LINE_LOG("[LINE] ready (I2S1 channels, task and buffers are created on start()); "
           "internal RAM free=%u B, largest block=%u B\n",
           static_cast<unsigned>(heapFree()), static_cast<unsigned>(heapLargest()));
  return true;
}

bool LinePath::isReady() {
  return s_ready;
}

bool LinePath::start() {
  if (!s_ready || __atomic_exchange_n(&s_opLock, true, __ATOMIC_ACQ_REL)) {
    return false;
  }
  const bool ok = startLocked(true);
  __atomic_store_n(&s_opLock, false, __ATOMIC_RELEASE);
  return ok;
}

bool LinePath::requestStart() {
  if (!s_ready || __atomic_exchange_n(&s_opLock, true, __ATOMIC_ACQ_REL)) {
    return false;
  }
  const bool ok = startLocked(false);
  __atomic_store_n(&s_opLock, false, __ATOMIC_RELEASE);
  return ok;
}

bool LinePath::stop() {
  if (!s_ready || __atomic_exchange_n(&s_opLock, true, __ATOMIC_ACQ_REL)) {
    return false;
  }
  bool ok = false;
  if (s_taskAlive && s_req == kReqNone) {
    s_req = kReqStop;
    ok = true;
  }
  __atomic_store_n(&s_opLock, false, __ATOMIC_RELEASE);
  return ok;
}

bool LinePath::returnToRadio() {
  if (!s_ready || __atomic_exchange_n(&s_opLock, true, __ATOMIC_ACQ_REL)) {
    return false;
  }
  bool ok = false;
  if (s_req == kReqNone) {
    ok = true;
    if (s_taskAlive) {
      s_req = kReqRadio;
    } else {
      LINE_LOG("[LINE] radio: resources already released\n");
      if (AudioPlayer::fixedOutputRate()) {
        AudioPlayer::setFixedOutputRate(false);
      }
    }
  }
  __atomic_store_n(&s_opLock, false, __ATOMIC_RELEASE);
  return ok;
}

bool LinePath::isRunning() {
  const LinePathState st = s_state;
  return st == LinePathState::Starting || st == LinePathState::Running || st == LinePathState::Stalled;
}

bool LinePath::isStable() {
  return s_state == LinePathState::Running;
}

bool LinePath::resourcesUp() {
  return s_taskAlive;
}

LinePathState LinePath::state() {
  return s_state;
}

const char* LinePath::stateName(LinePathState s) {
  switch (s) {
    case LinePathState::Off:      return "Off";
    case LinePathState::Starting: return "Starting";
    case LinePathState::Running:  return "Running";
    case LinePathState::Stalled:  return "Stalled";
    case LinePathState::Stopping: return "Stopping";
  }
  return "?";
}

LinePathError LinePath::lastError() {
  return s_lastError;
}

const char* LinePath::errorName(LinePathError e) {
  return errorNameImpl(e);
}

void LinePath::setVolumeDb(float db) {
  if (!(db >= line_path_cfg::kVolMinDb)) {  // також NaN
    db = line_path_cfg::kVolMinDb;
  }
  if (db > line_path_cfg::kVolMaxDb) {
    db = line_path_cfg::kVolMaxDb;
  }
  s_dsp.setAttenuationDb(db);
}

float LinePath::volumeDb() {
  return s_dsp.attenuationDb();
}

void LinePath::setAttenuationDb(int8_t db) {
  if (db < line_path_cfg::kAttMinDb) db = line_path_cfg::kAttMinDb;
  if (db > line_path_cfg::kAttMaxDb) db = line_path_cfg::kAttMaxDb;
  setVolumeDb(static_cast<float>(db));
}

int8_t LinePath::attenuationDb() {
  return static_cast<int8_t>(lroundf(s_dsp.attenuationDb()));
}

void LinePath::setUserMute(bool mute) {
  portENTER_CRITICAL(&s_muxMute);
  s_userMute = mute;
  portEXIT_CRITICAL(&s_muxMute);
  applyMute();
}

bool LinePath::userMuted() {
  return s_userMute;
}

// [P48b] Баланс — атомарне слово в DspVolume; перехід виконує задача line_path у наступному блоці.
void LinePath::setBalanceDb(int8_t db) {
  s_dsp.setBalanceDb(db);
}

int8_t LinePath::balanceDb() {
  return s_dsp.balanceDb();
}

void LinePath::setEqBandDb(uint8_t band, int8_t db) {
  s_eqLine.setBandDb(band, db);
}

int8_t LinePath::getEqBandDb(uint8_t band) {
  return s_eqLine.getBandDb(band);
}

void LinePath::setEqAllDb(const int8_t gains[eq_cfg::kBandCount]) {
  s_eqLine.setAllDb(gains);
}

void LinePath::getEqAllDb(int8_t out[eq_cfg::kBandCount]) {
  s_eqLine.getAllDb(out);
}

void LinePath::setEqBypass(bool on) {
  s_eqLine.setBypass(on);
}

bool LinePath::eqBypass() {
  return s_eqLine.bypass();
}

LinePathStats LinePath::stats() {
  LinePathStats s = {};
  s.state = s_state;
  s.muted = s_muted;
  s.routedToLine = s_routed;
  s.attenuationDb = attenuationDb();
  s.blocks = s_blocks;
  s.readTimeouts = s_readTimeouts;
  s.readErrors = s_readErrors;
  s.writeTimeouts = s_writeTimeouts;
  s.rxOverflows = s_rxOvf;
  s.txOverflows = s_txOvf;
  s.stalls = s_stalls;
  s.starts = s_starts;
  s.maxProcessUs = s_maxProcessUs;
  s.maxReadWaitUs = s_maxReadWaitUs;
  s.initMs = s_initMs;
  s.lastStartMs = s_lastStartMs;
  s.peakLeftDbfs = (s_peakL > 0.0f) ? 20.0f * log10f(s_peakL) : -99.0f;
  s.peakRightDbfs = (s_peakR > 0.0f) ? 20.0f * log10f(s_peakR) : -99.0f;
  s.peakOutLeftDbfs = (s_peakOutL > 0.0f) ? 20.0f * log10f(s_peakOutL) : -99.0f;
  s.peakOutRightDbfs = (s_peakOutR > 0.0f) ? 20.0f * log10f(s_peakOutR) : -99.0f;
  s.allocFailures = s_allocFailures;
  s.clipCount = s_clips;  // [P47d] лише фінальне насичення (після гучності)
  s.volumeDb = s_dsp.attenuationDb();
  s.userMuted = s_userMute;
  s.lastError = s_lastError;
  return s;
}

void LinePath::resetStats() {
  s_blocks = s_readTimeouts = s_readErrors = s_writeTimeouts = 0;
  s_stalls = s_starts = s_maxProcessUs = s_maxReadWaitUs = 0;
  s_allocFailures = s_recoveries = 0;
  s_rxOvf = 0;
  s_txOvf = 0;
  s_peakL = 0.0f;
  s_peakR = 0.0f;
  s_peakOutL = 0.0f;
  s_peakOutR = 0.0f;
  s_clips = 0;
#if ENABLE_VU
  s_vuMaxL = 0;
  s_vuMaxR = 0;
#endif
}

#if LINE_PATH_DIAG
void LinePath::diagSetBypass(bool on) {
  s_diagBypass = on;
  if (on) {
    s_eqLine.reset(); // Скидаємо стан фільтрів
  }
}
bool LinePath::diagBypass() { return s_diagBypass; }

void LinePath::diagSetTone(float hz, float dbfs) {
  if (hz <= 0.0f) {
    s_diagToneActive = false;
    s_diagToneHz = 0.0f;
    return;
  }
  s_diagToneHz = hz;
  s_diagToneAmp = powf(10.0f, dbfs / 20.0f);
  s_diagTonePhaseStep = 2.0f * 3.14159265358979323846f * hz / static_cast<float>(cfg::kFsHz);
  s_diagTonePhaseL = 0.0f;
  s_diagTonePhaseR = 0.0f;
  s_diagToneActive = true;
}
bool LinePath::diagToneActive() { return s_diagToneActive; }
float LinePath::diagToneHz() { return s_diagToneHz; }
#endif

#else  // LINE_PATH_ENABLE

// Модуль вимкнено: лише Serial-тест (lp.heap). Усі методи — безпечні заглушки.
bool LinePath::begin() {
  startTestTask();
  return false;
}
bool LinePath::isReady() { return false; }
bool LinePath::start() { return false; }
bool LinePath::requestStart() { return false; }
bool LinePath::stop() { return false; }
bool LinePath::returnToRadio() { return false; }
bool LinePath::isRunning() { return false; }
bool LinePath::isStable() { return false; }
bool LinePath::resourcesUp() { return false; }
LinePathState LinePath::state() { return LinePathState::Off; }
const char* LinePath::stateName(LinePathState) { return "Off"; }
LinePathError LinePath::lastError() { return LinePathError::None; }
const char* LinePath::errorName(LinePathError e) { return errorNameImpl(e); }
void LinePath::setVolumeDb(float) {}
float LinePath::volumeDb() { return line_path_cfg::kStartVolumeDb; }
void LinePath::setAttenuationDb(int8_t) {}
int8_t LinePath::attenuationDb() { return static_cast<int8_t>(line_path_cfg::kStartVolumeDb); }
void LinePath::setUserMute(bool) {}
bool LinePath::userMuted() { return true; }
void LinePath::setBalanceDb(int8_t) {}   // [P48b]
int8_t LinePath::balanceDb() { return 0; }
void LinePath::setEqBandDb(uint8_t, int8_t) {}
int8_t LinePath::getEqBandDb(uint8_t) { return 0; }
void LinePath::setEqAllDb(const int8_t[eq_cfg::kBandCount]) {}
void LinePath::getEqAllDb(int8_t out[eq_cfg::kBandCount]) {
  for (uint8_t b = 0; b < eq_cfg::kBandCount; ++b) out[b] = 0;
}
void LinePath::setEqBypass(bool) {}
bool LinePath::eqBypass() { return false; }
LinePathStats LinePath::stats() {
  LinePathStats s = {};
  s.volumeDb = line_path_cfg::kStartVolumeDb;
  s.userMuted = true;
  return s;
}
void LinePath::resetStats() {}

#endif  // LINE_PATH_ENABLE