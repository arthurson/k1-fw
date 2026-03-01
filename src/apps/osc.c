#include "osc.h"
#include "../board.h"
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
#include <string.h>

#define SMALL_FONT_H 6
#define OSC_TOP_MARGIN 24
#define OSC_GRAPH_H (LCD_HEIGHT - OSC_TOP_MARGIN - 1)
#define MAX_VAL 4095u
#define FFT_BINS 64
#define PULSE_BUF_SIZE 255

// ---------------------------------------------------------------------------
// Режимы отображения
// ---------------------------------------------------------------------------
typedef enum { MODE_WAVE = 0, MODE_FFT = 1, MODE_OOK = 2 } OscMode;

// ---------------------------------------------------------------------------
// Импульс OOK
// ---------------------------------------------------------------------------
typedef struct {
  uint16_t duration; // длительность в сэмплах ADC
  uint8_t high;      // 1=высокий, 0=низкий
} Pulse;

// ---------------------------------------------------------------------------
// Контекст
// ---------------------------------------------------------------------------
typedef struct {
  // --- WAVE ---
  uint8_t disp_buf[LCD_WIDTH];
  uint8_t disp_head;

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

// ---------------------------------------------------------------------------
// Таблица Ханна (Q15, 128 точек, максимум 32767 в центре)
// ---------------------------------------------------------------------------
static const uint16_t hann128[128] = {
    0,     20,    79,    178,   315,   492,   707,   961,   1252,  1580,  1945,
    2345,  2780,  3248,  3749,  4282,  4845,  5438,  6059,  6708,  7382,  8081,
    8803,  9546,  10309, 11090, 11888, 12700, 13526, 14363, 15209, 16063, 16923,
    17787, 18652, 19518, 20381, 21240, 22093, 22937, 23771, 24592, 25398, 26186,
    26955, 27703, 28427, 29125, 29797, 30439, 31050, 31628, 32173, 32682, 32767,
    32767, 32767, 32682, 32173, 31628, 31050, 30439, 29797, 29125, 28427, 27703,
    26955, 26186, 25398, 24592, 23771, 22937, 22093, 21240, 20381, 19518, 18652,
    17787, 16923, 16063, 15209, 14363, 13526, 12700, 11888, 11090, 10309, 9546,
    8803,  8081,  7382,  6708,  6059,  5438,  4845,  4282,  3749,  3248,  2780,
    2345,  1945,  1580,  1252,  961,   707,   492,   315,   178,   79,    20,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,
};

// test_tone
#include "../driver/gpio.h" // AUDIO_AudioPathOn/Off

// LUT синуса, 32 точки, амплитуда ±1800 отсчётов вокруг 2048
static const int16_t sine32[32] = {
    0,     352,  680,  963,  1188, 1340, 1413,  1402,  1308,  1137,  899,
    608,   282,  -60,  -400, -718, -992, -1204, -1341, -1402, -1385, -1293,
    -1133, -916, -655, -370, -78,  208,  471,   693,   856,   945,
};

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
  memset(osc.disp_buf, val_to_y(2048), sizeof(osc.disp_buf));
  osc.disp_head = 0;
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
    osc.mode = (OscMode)((osc.mode + 1) % 3);
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
    osc.disp_buf[osc.disp_head] = val_to_y((uint16_t)v);
    osc.disp_head = (uint8_t)((osc.disp_head + 1) % LCD_WIDTH);
  }

  // FFT: накапливаем сырые uint16_t напрямую в память fft_re[]
  if (osc.fft_acc_pos < 128) {
    ((uint16_t *)fft_re)[osc.fft_acc_pos++] = raw;
  }

  if (osc.fft_acc_pos == 128) {
    uint16_t dc_snap = (uint16_t)(osc.dc_iir >> 8);

    for (int i = 0; i < 128; i++) {
      int32_t v = (int32_t)((uint16_t *)fft_re)[i] - dc_snap;
      if (v > 32767)
        v = 32767;
      if (v < -32767)
        v = -32767;
      fft_re[i] = (int16_t)v;
      fft_im[i] = 0;
    }

    // Окно Ханна
    for (int i = 0; i < 128; i++) {
      fft_re[i] = (int16_t)((int32_t)fft_re[i] * hann128[i] >> 15);
    }

    FFT_128(fft_re, fft_im);

    {
      static uint16_t mag_tmp[FFT_BINS];
      FFT_Magnitude(fft_re, fft_im, mag_tmp, FFT_BINS);
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
// Отрисовка — WAVE
// ---------------------------------------------------------------------------
static void drawWaveform(void) {
  int prev_y = osc.disp_buf[osc.disp_head];
  for (int x = 1; x < LCD_WIDTH; x++) {
    uint8_t idx = (uint8_t)((osc.disp_head + x) % LCD_WIDTH);
    int y = osc.disp_buf[idx];
    DrawLine(x - 1, prev_y, x, y, C_FILL);
    prev_y = y;
  }
  // Линия идеального центра (2048) — пунктиром
  {
    int cy = val_to_y(2048);
    for (int x = 0; x < LCD_WIDTH; x += 6)
      PutPixel(x, cy, C_FILL);
  }
}

static void drawTriggerMarker(void) {
  if (!osc.show_trigger)
    return;
  int y = val_to_y(osc.trigger_level);
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
// Отрисовка — метрики качества сигнала (VU-бар, DC, клиппинг)
// Занимает строки 3-4 в шапке (y = SMALL_FONT_H*3 и SMALL_FONT_H*4-1)
// ---------------------------------------------------------------------------
#define SIG_FULL_AMP 2048u // полная амплитуда = Vref/2

static void drawSignalInfo(void) {
  // --- Уровень в процентах (0..100%) ---
  uint8_t level_pct = (uint8_t)((uint32_t)osc.sig_amp * 100u / SIG_FULL_AMP);
  if (level_pct > 100)
    level_pct = 100;

  // --- DC-смещение от идеального центра ---
  int16_t dc_err = (int16_t)osc.sig_mid - 2048;

  // --- VU-бар: y-позиция строки 3 (y=18), высота 4 пикселя ---
  // Ширина бара = LCD_WIDTH - 28 (оставляем 28px слева для текста)
  const int BAR_X = 28; // начало бара
  const int BAR_W = LCD_WIDTH - BAR_X - 1;
  const int BAR_Y = SMALL_FONT_H * 3 - 4; // верх бара
  const int BAR_H = 4;

  // Рамка
  DrawLine(BAR_X, BAR_Y, BAR_X + BAR_W, BAR_Y, C_FILL);
  DrawLine(BAR_X, BAR_Y + BAR_H, BAR_X + BAR_W, BAR_Y + BAR_H, C_FILL);
  DrawLine(BAR_X, BAR_Y, BAR_X, BAR_Y + BAR_H, C_FILL);
  DrawLine(BAR_X + BAR_W, BAR_Y, BAR_X + BAR_W, BAR_Y + BAR_H, C_FILL);

  // Заливка уровня
  int fill_w = (int)((uint32_t)level_pct * (BAR_W - 2) / 100);
  if (fill_w > 0)
    FillRect(BAR_X + 1, BAR_Y + 1, fill_w, BAR_H - 1, C_FILL);

  // Маркер зоны предклиппинга (~87%)
  int warn_x = BAR_X + 1 + (BAR_W - 2) * 87 / 100;
  PutPixel(warn_x, BAR_Y, C_FILL);
  PutPixel(warn_x, BAR_Y + BAR_H, C_FILL);

  // --- Текст: уровень% слева от бара ---
  PrintSmallEx(BAR_X - 1, SMALL_FONT_H * 3, POS_R, C_FILL, "%3u%%", level_pct);

  // --- Строка 4: DC-смещение | CLIP ---
  if (dc_err >= 0)
    PrintSmallEx(0, SMALL_FONT_H * 4, POS_L, C_FILL, "DC+%d", dc_err);
  else
    PrintSmallEx(0, SMALL_FONT_H * 4, POS_L, C_FILL, "DC%d", dc_err);

  if (osc.clip_flag)
    PrintSmallEx(LCD_WIDTH, SMALL_FONT_H * 4, POS_R, C_FILL, "!CLIP!");
}

// ---------------------------------------------------------------------------
// Отрисовка — статус
// ---------------------------------------------------------------------------
static void drawStatus(void) {
  char buf[16];
  mhzToS(buf, RADIO_GetParam(ctx, PARAM_FREQUENCY));

  const char *mode_str = osc.mode == MODE_FFT   ? "FFT"
                         : osc.mode == MODE_OOK ? "OOK"
                                                : "OSC";
  // Строка 2: режим | частота
  PrintSmallEx(0, SMALL_FONT_H * 2, POS_L, C_FILL, "%s", mode_str);
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
