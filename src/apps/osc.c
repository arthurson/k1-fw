#include "osc.h"
#include "../board.h"
#include "../driver/gpio.h" // AUDIO_AudioPathOn/Off
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/audio_io.h"
#include "../helper/fft.h"
#include "../helper/ook.h"
#include "../helper/regs-menu.h"
#include "../settings.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SMALL_FONT_H 6
#define OSC_TOP_MARGIN 24
#define OSC_GRAPH_H (LCD_HEIGHT - OSC_TOP_MARGIN - 1)
#define MAX_VAL 4095u
#define FFT_BINS 64

// ---------------------------------------------------------------------------
// Режимы отображения
// ---------------------------------------------------------------------------
typedef enum {
  MODE_WAVE,
  MODE_FFT,
  MODE_OOK,

  MODE_COUNT,
} OscMode;

// ---------------------------------------------------------------------------
// Контекст
// ---------------------------------------------------------------------------
typedef struct {
  // --- WAVE ---
  uint16_t
      disp_buf[LCD_WIDTH]; // хранит масштабированные значения АЦП (0..4095)
  uint8_t disp_head;
  uint16_t disp_vmin; // авто-диапазон для текущего кадра
  uint16_t disp_vmax;

  // --- FFT ---
  uint8_t fft_acc_pos;
  uint8_t fft_mag[FFT_BINS];
  bool fft_fresh;

  // --- Общие ---
  OscMode mode;
  uint8_t scale_v;
  uint8_t scale_t;
  uint16_t trigger_level;

  bool dc_offset;
  bool show_grid;
  bool show_trigger;

  uint32_t dc_iir;
  uint8_t decimate_cnt;
  int32_t lpf_iir;

  // --- Качество сигнала (обновляется в process_block) ---
  uint16_t sig_amp; // амплитуда = (max-min)/2, идеал ~1800..2000
  uint16_t sig_mid; // DC-центр  = min + amp,    идеал ~2048
  bool clip_flag;   // true если касается 0 или 4095
} OscContext;

static OscContext osc;
static uint16_t dmaMin;
static uint16_t dmaMax;

// ---------------------------------------------------------------------------
// FFT-буферы
// ---------------------------------------------------------------------------
static int16_t fft_re[128];
static int16_t fft_im[128];

static const int16_t sine32[32] = {
    0,     351,   689,   1000,  1273,  1497,  1663,  1765,  1800,  1765,  1663,
    1497,  1273,  1000,  689,   351,   0,     -351,  -689,  -1000, -1273, -1497,
    -1663, -1765, -1800, -1765, -1663, -1497, -1273, -1000, -689,  -351};

static uint32_t tone_phase = 0;

// 440 Гц при Fs=9600: шаг фазы = 440 * 32 / 9600 ≈ 1.467
// Используем fixed-point 8.8: шаг = (440 * 32 * 256) / 9600 = 375
#define TONE_PHASE_STEP 375u // Q8

static uint32_t tone_source(uint16_t *buf, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    int16_t s = sine32[(tone_phase >> 8) & 31];
    buf[i] = (uint16_t)(2048 + s);
    tone_phase += TONE_PHASE_STEP;
  }
  return n; // бесконечный источник
}

void TEST_TONE_Start(void) {
  tone_phase = 0;
  GPIO_EnableAudioPath();
  AUDIO_IO_SourceSet(tone_source);
  LogC(LOG_C_BRIGHT_WHITE, "TEST: 440Hz tone started on PA4");
}

void TEST_TONE_Stop(void) {
  AUDIO_IO_SourceClear();
  GPIO_DisableAudioPath();
  LogC(LOG_C_BRIGHT_WHITE, "TEST: tone stopped");
}

// ---------------------------------------------------------------------------
// val_to_y
// ---------------------------------------------------------------------------
static inline uint8_t val_to_y(uint16_t val) {
  int y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 -
          (int32_t)val * (OSC_GRAPH_H - 1) / MAX_VAL;
  if (y < OSC_TOP_MARGIN)
    y = OSC_TOP_MARGIN;
  if (y >= OSC_TOP_MARGIN + OSC_GRAPH_H)
    y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1;
  return (uint8_t)y;
}

// ---------------------------------------------------------------------------
// Вспомогательные / параметры
// ---------------------------------------------------------------------------
static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
}

static void setScaleV(uint32_t v, uint32_t _) {
  (void)_;
  if (v < 1)
    v = 1;
  if (v > 64)
    v = 64;
  osc.scale_v = (uint8_t)v;
}

static void setScaleT(uint32_t v, uint32_t _) {
  (void)_;
  if (v < 1)
    v = 1;
  if (v > 128)
    v = 128;
  osc.scale_t = (uint8_t)v;
}

static void setTriggerLevel(uint32_t v, uint32_t _) {
  (void)_;
  if (v > MAX_VAL)
    v = MAX_VAL;
  osc.trigger_level = (uint16_t)v;
}

static void triggerArm(void) {
  for (int i = 0; i < LCD_WIDTH; i++)
    osc.disp_buf[i] = 2048;
  osc.disp_head = 0;
  osc.disp_vmin = 0;
  osc.disp_vmax = MAX_VAL;
  osc.decimate_cnt = 0;
  osc.fft_acc_pos = 0;
  osc.fft_fresh = false;
  osc.dc_iir = 2048UL << 8;
  osc.lpf_iir = 2048L << 8;
}

// ---------------------------------------------------------------------------
// Клавиши
// ---------------------------------------------------------------------------
bool OSC_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state))
    return true;
  if (state != KEY_RELEASED && state != KEY_LONG_PRESSED_CONT)
    return false;

  switch (key) {
  // Вертикальный масштаб / порог OOK вверх
  case KEY_2:
    if (osc.mode == MODE_OOK) {
    } else {
      setScaleV(osc.scale_v + 1, 0);
    }
    return true;
  // Вертикальный масштаб / порог OOK вниз
  case KEY_8:
    if (osc.mode == MODE_OOK) {
    } else {
      setScaleV(osc.scale_v - 1, 0);
    }
    return true;

  case KEY_1:
    setScaleT(osc.scale_t + 1, 0);
    return true;
  case KEY_7:
    setScaleT(osc.scale_t - 1, 0);
    return true;

  case KEY_3:
    setTriggerLevel(osc.trigger_level + 128, 0);
    return true;
  case KEY_9:
    setTriggerLevel(osc.trigger_level - 128, 0);
    return true;

  case KEY_4:
    osc.dc_offset = !osc.dc_offset;
    triggerArm();
    return true;

  case KEY_F:
    osc.show_grid = !osc.show_grid;
    return true;

  case KEY_5:
    FINPUT_setup(BK4819_F_MIN, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(tuneTo);
    return true;

  case KEY_0:
    FINPUT_setup(0, MAX_VAL, UNIT_RAW, false);
    FINPUT_Show(setTriggerLevel);
    return true;
  case KEY_SIDE1:
    BK4819_ToggleAFDAC(false);
    BK4819_ToggleAFBit(false);
    TEST_TONE_Start();
    return true;
  case KEY_SIDE2: {
    uint16_t reg43 = BK4819_ReadRegister(0x43); // 15
    if ((reg43 >> 15) & 1) {
      reg43 &= ~(1 << 15);
    } else {
      reg43 |= 1 << 15;
    }

    BK4819_WriteRegister(0x43, reg43);
  }
    return true;

  // Переключение режимов: WAVE → FFT → OOK → WAVE
  case KEY_6:
    osc.mode = IncDecU(osc.mode, 0, MODE_COUNT, true);
    return true;

  case KEY_STAR:
    triggerArm();
    return true;

  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// push_sample — WAVE + FFT
// ---------------------------------------------------------------------------
static void push_sample(uint16_t raw) {
  // IIR DC-фильтр
  osc.dc_iir += (int32_t)raw - (int32_t)(osc.dc_iir >> 8);
  uint16_t dc = (uint16_t)(osc.dc_iir >> 8);

  // WAVE: вычисляем Y-пиксель
  {
    int32_t v = osc.dc_offset ? ((int32_t)raw - dc) * osc.scale_v / 10 + 2048
                              : ((int32_t)raw - 2048) * osc.scale_v / 10 + 2048;
    if (v < 0)
      v = 0;
    if (v > (int32_t)MAX_VAL)
      v = MAX_VAL;
    osc.disp_buf[osc.disp_head] =
        (uint16_t)v; // сырое значение, Y считается при отрисовке
    osc.disp_head = (uint8_t)((osc.disp_head + 1) % LCD_WIDTH);
  }

  // FFT: накапливаем сырые uint16_t напрямую в память fft_re[]
  if (osc.fft_acc_pos < 128) {
    ((uint16_t *)fft_re)[osc.fft_acc_pos++] = raw;
  }

  if (osc.fft_acc_pos == 128) {
    FFT_RemoveDC(fft_re);
    FFT_ApplyWindow(fft_re);
    FFT_Forward(fft_re, fft_im);

    {
      static uint16_t mag_tmp[FFT_BINS];
      FFT_MagnitudeFast(fft_re, fft_im, mag_tmp, FFT_BINS);
      for (int k = 0; k < FFT_BINS; k++) {
        uint16_t v = mag_tmp[k] >> 1;
        osc.fft_mag[k] = v > 255 ? 255 : (uint8_t)v;
      }
    }

    osc.fft_fresh = true;
    osc.fft_acc_pos = 0;
  }
}

// ---------------------------------------------------------------------------
// process_block
// ---------------------------------------------------------------------------
static void process_block(const volatile uint16_t *src, uint32_t len) {
  dmaMin = UINT16_MAX;
  dmaMax = 0;
  for (uint32_t i = 0; i < len; i++) {
    uint16_t s = src[i];
    if (s > dmaMax)
      dmaMax = s;
    if (s < dmaMin)
      dmaMin = s;

    if (++osc.decimate_cnt >= osc.scale_t) {
      osc.decimate_cnt = 0;
      push_sample(s);
      if (osc.mode == MODE_FFT)
        gRedrawScreen = true;
    }
  }

  // Обновляем метрики качества сигнала
  uint16_t amp = (dmaMax - dmaMin) / 2;
  osc.sig_amp = amp;
  osc.sig_mid = dmaMin + amp;
  // Клиппинг: касание краёв ADC с порогом 8 отсчётов
  osc.clip_flag = (dmaMin <= 8) || (dmaMax >= 4087);
}

static void osc_sink(const uint16_t *buf, uint32_t n) {
  process_block(buf, n); // та же логика, но buf уже валиден
  ook_sink(buf, n);
}

uint8_t ookData[128];
uint16_t ookLen;

void myOokHandler(const uint8_t *data, uint16_t nbytes) {
  if (nbytes > 0) {
    for (uint16_t i = 0; i < nbytes; ++i) {
      ookData[i] = data[i];
    }
    ookLen = nbytes;
  }
}

// ---------------------------------------------------------------------------
// Инициализация
// ---------------------------------------------------------------------------
void OSC_init(void) {
  osc.mode = MODE_WAVE;
  osc.scale_v = 10;
  osc.scale_t = 2;
  osc.trigger_level = 2048;
  osc.dc_offset = false;
  osc.show_grid = false;
  osc.show_trigger = true;
  triggerArm();
  ook_reset();
  ookHandler = myOokHandler;
  AUDIO_IO_SinkRegister(osc_sink);
  // AUDIO_IO_SinkRegister(ook_sink);
}

void OSC_deinit(void) { AUDIO_IO_SinkUnregister(osc_sink); }

void OSC_update(void) {}

// ---------------------------------------------------------------------------
// Отрисовка — сетка
// ---------------------------------------------------------------------------
static void drawGrid(void) {
  if (!osc.show_grid)
    return;
  for (int i = 0; i <= 4; i++) {
    int y = OSC_TOP_MARGIN + (OSC_GRAPH_H * i) / 4;
    for (int x = 0; x < LCD_WIDTH; x += 4)
      PutPixel(x, y, C_FILL);
  }
  for (int i = 0; i <= 8; i++) {
    int x = (LCD_WIDTH * i) / 8;
    for (int y = OSC_TOP_MARGIN; y < OSC_TOP_MARGIN + OSC_GRAPH_H; y += 4)
      PutPixel(x, y, C_FILL);
  }
}

// ---------------------------------------------------------------------------
// val_to_y_ranged — маппинг [vmin..vmax] на пиксели экрана
// ---------------------------------------------------------------------------
static inline uint8_t val_to_y_ranged(uint16_t val, uint16_t vmin,
                                      uint16_t vmax) {
  if (vmax <= vmin)
    return (uint8_t)(OSC_TOP_MARGIN + OSC_GRAPH_H / 2);
  int32_t range = vmax - vmin;
  int y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 -
          (int32_t)(val - vmin) * (OSC_GRAPH_H - 1) / range;
  if (y < OSC_TOP_MARGIN)
    y = OSC_TOP_MARGIN;
  if (y >= OSC_TOP_MARGIN + OSC_GRAPH_H)
    y = OSC_TOP_MARGIN + OSC_GRAPH_H - 1;
  return (uint8_t)y;
}

// ---------------------------------------------------------------------------
// Отрисовка — WAVE  (авто-масштаб: сигнал всегда заполняет экран)
// ---------------------------------------------------------------------------
static void drawWaveform(void) {
  // Сканируем буфер для авто-диапазона
  uint16_t vmin = MAX_VAL, vmax = 0;
  for (int i = 0; i < LCD_WIDTH; i++) {
    if (osc.disp_buf[i] < vmin)
      vmin = osc.disp_buf[i];
    if (osc.disp_buf[i] > vmax)
      vmax = osc.disp_buf[i];
  }
  // Паддинг ~12% чтобы сигнал не упирался в края
  uint16_t span = vmax - vmin;
  uint16_t pad = span / 8 + 16;
  vmin = (vmin > pad) ? vmin - pad : 0;
  vmax = (vmax + pad <= MAX_VAL) ? vmax + pad : MAX_VAL;
  // Минимальный диапазон чтобы не делить на ноль при постоянном DC
  if ((uint16_t)(vmax - vmin) < 60) {
    uint16_t mid = (uint16_t)((vmin + vmax) / 2);
    vmin = (mid >= 30) ? mid - 30 : 0;
    vmax = (mid + 30 <= MAX_VAL) ? mid + 30 : MAX_VAL;
  }
  osc.disp_vmin = vmin;
  osc.disp_vmax = vmax;

  int prev_y = val_to_y_ranged(osc.disp_buf[osc.disp_head], vmin, vmax);
  for (int x = 1; x < LCD_WIDTH; x++) {
    uint8_t idx = (uint8_t)((osc.disp_head + x) % LCD_WIDTH);
    int y = val_to_y_ranged(osc.disp_buf[idx], vmin, vmax);
    DrawLine(x - 1, prev_y, x, y, C_FILL);
    prev_y = y;
  }
  // Пунктир на уровне 0V (GND = ADC 0) и 2048 (Vref/2)
  {
    int cy = val_to_y_ranged(2048, vmin, vmax);
    for (int x = 0; x < LCD_WIDTH; x += 6)
      PutPixel(x, cy, C_FILL);
  }
}

static void drawTriggerMarker(void) {
  if (!osc.show_trigger)
    return;
  int y = val_to_y_ranged(osc.trigger_level, osc.disp_vmin, osc.disp_vmax);
  for (int i = 0; i < 3; i++) {
    PutPixel(i, y, C_FILL);
    PutPixel(LCD_WIDTH - 1 - i, y, C_FILL);
  }
  PutPixel(1, y - 1, C_FILL);
  PutPixel(1, y + 1, C_FILL);
  PutPixel(LCD_WIDTH - 2, y - 1, C_FILL);
  PutPixel(LCD_WIDTH - 2, y + 1, C_FILL);
}

// ---------------------------------------------------------------------------
// Отрисовка — FFT
// ---------------------------------------------------------------------------
static void drawSpectrum(void) {
  if (!osc.fft_fresh) {
    PrintSmallEx(LCD_XCENTER, OSC_TOP_MARGIN + OSC_GRAPH_H / 2, POS_C, C_FILL,
                 "FFT...");
    return;
  }

  uint8_t peak_mag = 8;
  uint8_t peak_bin = 1;
  for (int k = 1; k < FFT_BINS; k++) {
    if (osc.fft_mag[k] > peak_mag) {
      peak_mag = osc.fft_mag[k];
      peak_bin = (uint8_t)k;
    }
  }

  for (int k = 0; k < FFT_BINS; k++) {
    uint32_t h = (uint32_t)osc.fft_mag[k] * (OSC_GRAPH_H - 1) / peak_mag;
    if (h > (uint32_t)(OSC_GRAPH_H - 1))
      h = OSC_GRAPH_H - 1;
    int x0 = k * 2;
    int y_top = OSC_TOP_MARGIN + OSC_GRAPH_H - 1 - (int)h;
    DrawLine(x0, y_top, x0, OSC_TOP_MARGIN + OSC_GRAPH_H - 1, C_FILL);
    DrawLine(x0 + 1, y_top, x0 + 1, OSC_TOP_MARGIN + OSC_GRAPH_H - 1, C_FILL);
  }

  // Стрелка над пиком
  int px = peak_bin * 2 + 1;
  if (px > 0 && px < LCD_WIDTH - 1) {
    PutPixel(px, OSC_TOP_MARGIN, C_FILL);
    PutPixel(px - 1, OSC_TOP_MARGIN + 1, C_FILL);
    PutPixel(px + 1, OSC_TOP_MARGIN + 1, C_FILL);
  }

#define ADC_FS_HZ 9600U
  // Частота пика.
  // Эффективная Fs после децимации = ADC_FS_HZ / scale_t
  // Разрешение по частоте = Fs_eff / 128
  // F_peak = peak_bin * ADC_FS_HZ / (scale_t * 128)
  uint32_t fs_eff = ADC_FS_HZ / (uint32_t)osc.scale_t; // Гц
  uint32_t peak_hz = (uint32_t)peak_bin * fs_eff / 128U;

  if (peak_hz < 1000) {
    PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 3, POS_C, C_FILL, "Pk:%luHz",
                 peak_hz);
  } else {
    // Одна цифра после запятой: X.Y kHz
    uint32_t khz_int = peak_hz / 1000;
    uint32_t khz_frac = (peak_hz % 1000) / 100;
    PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 3, POS_C, C_FILL, "Pk:%lu.%lukHz",
                 khz_int, khz_frac);
  }
}

// ---------------------------------------------------------------------------
// Отрисовка — OOK
// ---------------------------------------------------------------------------
static void drawOOK(void) {
  PrintSmallEx(0, 24, POS_L, C_FILL, "LEN: %u", ookLen);
  for (uint16_t i = 0; i < ookLen; ++i) {
    PrintSmallEx((i % 8) * 10, 32 + (i / 8) * 6, POS_L, C_FILL, "%02X",
                 ookData[i]);
  }
}

// ---------------------------------------------------------------------------
// Отрисовка — метрики сигнала в вольтах
// PY32F071: ADC 12-бит, Vref = 3.3V → 1 LSB = 3300/4095 мВ
// ---------------------------------------------------------------------------
#define VREF_MV 3300u

// Форматирует мВ как "X.XXV" или "XXXmV" в буфер
static void fmt_mv(char *buf, uint8_t buflen, uint32_t mv) {
  if (mv >= 1000)
    snprintf(buf, buflen, "%lu.%02luV", mv / 1000, (mv % 1000) / 10);
  else
    snprintf(buf, buflen, "%lumV", mv);
}

static void drawSignalInfo(void) {
  // Vpp = peak-to-peak по последнему DMA-блоку
  uint32_t vpp_mv = (uint32_t)(dmaMax - dmaMin) * VREF_MV / MAX_VAL;
  // Vdc = DC-центр сигнала
  uint32_t vdc_mv = (uint32_t)osc.sig_mid * VREF_MV / MAX_VAL;
  // Vmin / Vmax
  uint32_t vmin_mv = (uint32_t)dmaMin * VREF_MV / MAX_VAL;
  uint32_t vmax_mv = (uint32_t)dmaMax * VREF_MV / MAX_VAL;

  char sbuf[10];

  // Строка 3: Vpp слева,  Vdc справа
  fmt_mv(sbuf, sizeof(sbuf), vpp_mv);
  PrintSmallEx(0, SMALL_FONT_H * 3, POS_L, C_FILL, "Vpp:%s", sbuf);

  fmt_mv(sbuf, sizeof(sbuf), vdc_mv);
  PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 3, POS_R, C_FILL, "Vdc:%s", sbuf);

  // Строка 4: диапазон Vmin..Vmax | CLIP
  fmt_mv(sbuf, sizeof(sbuf), vmin_mv);
  char sbuf2[10];
  fmt_mv(sbuf2, sizeof(sbuf2), vmax_mv);
  PrintSmallEx(0, SMALL_FONT_H * 4, POS_L, C_FILL, "%s~%s", sbuf, sbuf2);

  if (osc.clip_flag)
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 4, POS_R, C_FILL, "!CLIP!");
}

// ---------------------------------------------------------------------------
// Отрисовка — статус
// ---------------------------------------------------------------------------
static void drawStatus(void) {
  char buf[16];
  mhzToS(buf, RADIO_GetParam(ctx, PARAM_FREQUENCY));

  // Строка 2: режим | частота
  PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL, "%s",
               (const char[][4]){"OSC", "FFT", "OOK"}[osc.mode]);
  PrintSmallEx(LCD_XCENTER, SMALL_FONT_H * 2, POS_C, C_FILL, "%s", buf);

  // Строка 3 (правый край): масштаб времени
  PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 2, POS_R, C_FILL, "T:%d", osc.scale_t);

  // Строка 1: DC/RAW | вертикальный масштаб (только WAVE)
  PrintSmallEx(0, SMALL_FONT_H, POS_L, C_FILL, osc.dc_offset ? "DC" : "RAW");
  if (osc.mode == MODE_WAVE)
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H, POS_R, C_FILL, "V:%d", osc.scale_v);
}

// ---------------------------------------------------------------------------
// Главная функция отрисовки
// ---------------------------------------------------------------------------
void OSC_render(void) {
  FillRect(0, OSC_TOP_MARGIN, LCD_WIDTH, OSC_GRAPH_H, C_CLEAR);
  STATUSLINE_RenderRadioSettings();

  drawGrid();

  if (osc.mode == MODE_WAVE) {
    drawWaveform();
    drawTriggerMarker();
    drawSignalInfo();
  } else if (osc.mode == MODE_FFT) {
    drawSpectrum();
  } else {
    drawOOK();
  }

  drawStatus();

  REGSMENU_Draw();
}
