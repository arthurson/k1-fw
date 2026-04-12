#include "spectrum.h"
#include "../board.h"
#include "../driver/uart.h"
#include "../helper/measurements.h"
#include "components.h"
#include "graphics.h"
#include <stdint.h>
#include <string.h>

#define MAX_POINTS 128

uint8_t SPECTRUM_Y = 8;
uint8_t SPECTRUM_H = 44;
GraphMeasurement graphMeasurement = GRAPH_RSSI;

static uint8_t  S_BOTTOM;
static uint16_t rssiHistory[MAX_POINTS]  = {0};
static uint8_t  noiseHistory[MAX_POINTS] = {0};
static uint8_t  glitchHistory[MAX_POINTS]= {0};
static uint8_t  x  = 0;
static uint8_t  ox = UINT8_MAX;
static uint8_t  filledPoints;
static Band    *range;
static uint16_t step;

// fallback-границы (до первого скана)
#define SP_DBM_MIN (-120)
#define SP_DBM_MAX  (-60)

#define DBM_SPAN  40  // дБ, высота шкалы

// текущие границы в дБм (пересчитываются в SP_Begin)
static int16_t spDbmMin = SP_DBM_MIN;
static int16_t spDbmMax = SP_DBM_MIN + DBM_SPAN;

static void updateDbmFloorEma(void) {
  if (filledPoints < 1) return;

  // минимальный RSSI в истории → нижняя граница шкалы
  uint16_t minRssi = rssiHistory[0];
  for (uint8_t i = 1; i < filledPoints; i++)
    if (rssiHistory[i] < minRssi) minRssi = rssiHistory[i];

  spDbmMin = Rssi2DBm(minRssi);
  spDbmMax = spDbmMin + DBM_SPAN;
}

// дБм → высота бара в пикселях [0..SPECTRUM_H]
static uint8_t dbm2Y(int16_t dbm, VMinMax v) {
  int16_t dbMin = Rssi2DBm(v.vMin);
  int16_t dbMax = Rssi2DBm(v.vMax);
  if (dbm <= dbMin) return 0;
  if (dbm >= dbMax) return SPECTRUM_H;
  uint8_t y = (uint8_t)((int32_t)(dbm - dbMin) * SPECTRUM_H
                        / (dbMax - dbMin));
  return y < 2 ? 2 : y;  // слабый сигнал — минимум 2 пикселя
}
/* static uint8_t dbm2Y(int16_t dbm) {
  if (dbm <= spDbmMin) return 0;
  if (dbm >= spDbmMax) return SPECTRUM_H;

  // линейно нормируем в [0 .. SPECTRUM_H²]
  uint32_t linear = (uint32_t)(dbm - spDbmMin)
                    * (SPECTRUM_H * SPECTRUM_H)
                    / (spDbmMax - spDbmMin);

  // целочисленный sqrt — цикл ≤ 44 итерации, ок для embedded
  uint8_t y = 0;
  while ((uint32_t)(y + 1) * (y + 1) <= linear) y++;
  return y;
} */
/* static uint8_t dbm2Y(int16_t dbm) {
  if (dbm <= spDbmMin) return 0;
  if (dbm >= spDbmMax) return SPECTRUM_H;
  return (uint8_t)((int32_t)(dbm - spDbmMin) * SPECTRUM_H
                   / (spDbmMax - spDbmMin));
} */

// ────────────────────────────────────────────────────────────────────

static void drawTicks(uint8_t y, uint32_t fs, uint32_t fe, uint32_t div,
                      uint8_t h) {
  for (uint32_t f = fs - (fs % div) + div; f < fe; f += div)
    DrawVLine(SP_F2X(f), y, h, C_FILL);
}

void UI_DrawTicks(uint8_t y, const Band *band) {
  uint32_t fs = band->start, fe = band->end, bw = fe - fs;
  for (uint32_t p = 100000000; p >= 10; p /= 10) {
    if (p < bw) {
      drawTicks(y, fs, fe, p / 2, 2);
      drawTicks(y, fs, fe, p, 3);
      return;
    }
  }
}

// ────────────────────────────────────────────────────────────────────

static uint8_t  visited[MAX_POINTS / 8 + 1] = {0};
static uint8_t  spPrevXc    = 0;
static uint32_t spPeakF     = 0;
static uint16_t spPeakRssi  = 0;
static uint32_t spPeakFDisp    = 0;
static uint16_t spPeakRssiDisp = 0;

void SP_ResetHistory(void) {
  filledPoints = 0;
  memset(rssiHistory,   0, sizeof(rssiHistory));
  memset(noiseHistory,  0, sizeof(noiseHistory));
  memset(glitchHistory, 0, sizeof(glitchHistory));
  memset(visited,       0, sizeof(visited));
  spDbmMin      = SP_DBM_MIN;
  spDbmMax      = SP_DBM_MIN + DBM_SPAN;
}

void SP_Begin(void) {
  x        = 0;
  ox       = UINT8_MAX;
  spPrevXc = 0;
  spPeakFDisp    = spPeakF;
  spPeakRssiDisp = spPeakRssi;
  spPeakF        = 0;
  spPeakRssi     = 0;
  memset(visited, 0, sizeof(visited));
  updateDbmFloorEma();
}

void SP_Init(Band *b) {
  S_BOTTOM = SPECTRUM_Y + SPECTRUM_H;
  range = b;
  step  = StepFrequencyTable[b->step];
  SP_ResetHistory();
  SP_Begin();
}

// ────────────────────────────────────────────────────────────────────

uint8_t SP_F2X(uint32_t f) {
  if (f <= range->start) return 0;
  if (f >= range->end)   return MAX_POINTS - 1;
  uint32_t delta  = f - range->start;
  uint32_t aRange = range->end - range->start;
  return (uint8_t)(((uint64_t)delta * (MAX_POINTS - 1) + aRange / 2) / aRange);
}

uint32_t SP_X2F(uint8_t xi) {
  if (xi >= MAX_POINTS) xi = MAX_POINTS - 1;
  uint32_t s = (range->end - range->start) / (MAX_POINTS - 1);
  return range->start + (xi * s);
}

// ────────────────────────────────────────────────────────────────────

#define _MIN(a, b) (((a) < (b)) ? (a) : (b))
#define _MAX(a, b) (((a) > (b)) ? (a) : (b))

void SP_AddPoint(const Measurement *msm) {
  uint8_t xc = SP_F2X(msm->f);
  uint8_t next_xc =
      (msm->f + step > range->end) ? (MAX_POINTS - 1) : SP_F2X(msm->f + step);

  int16_t ixs, ixe;
  if (msm->f == range->start) {
    ixs = 0;
    ixe = xc + (next_xc - xc) / 2;
  } else if (msm->f + step > range->end) {
    ixs = spPrevXc + ((int16_t)xc - spPrevXc) / 2;
    ixe = MAX_POINTS - 1;
  } else {
    ixs = spPrevXc + ((int16_t)xc - spPrevXc) / 2;
    ixe = xc + ((int16_t)next_xc - xc) / 2 - 1;
  }

  if (ixs > ixe) { int16_t t = ixs; ixs = ixe; ixe = t; }
  ixs = _MAX(0, ixs);
  ixe = _MIN(MAX_POINTS - 1, ixe);

  for (uint8_t xi = (uint8_t)ixs; xi <= (uint8_t)ixe; ++xi) {
    uint8_t byte = xi / 8, bit = xi % 8;
    if ((visited[byte] & (1 << bit)) == 0) {
      rssiHistory[xi]   = msm->rssi;
      noiseHistory[xi]  = msm->noise;
      glitchHistory[xi] = msm->glitch;
      visited[byte] |= (1 << bit);
    } else if (msm->rssi > rssiHistory[xi]) {
      rssiHistory[xi]   = msm->rssi;
      noiseHistory[xi]  = msm->noise;
      glitchHistory[xi] = msm->glitch;
    }
    if (xi + 1 > filledPoints)
      filledPoints = xi + 1;
  }
  spPrevXc = xc;

  if (msm->rssi > spPeakRssi) {
    spPeakRssi = msm->rssi;
    spPeakF    = msm->f;
  }
}

// ────────────────────────────────────────────────────────────────────

VMinMax SP_GetMinMax(void) {
  return (VMinMax){
      .vMin = DBm2Rssi(SP_DBM_MIN),
      .vMax = DBm2Rssi(SP_DBM_MAX),
  };
}

VMinMax SP_GetGraphMinMax(void) {
  switch (graphMeasurement) {
  case GRAPH_NOISE:  return (VMinMax){.vMin = 0, .vMax = 128};
  case GRAPH_GLITCH: return (VMinMax){.vMin = 0, .vMax = 255};
  default:           return SP_GetMinMax();
  }
}

// ────────────────────────────────────────────────────────────────────

uint8_t  SP_FindPeakX(void)   { uint32_t f = SP_GetPeakF(); return f ? SP_F2X(f) : MAX_POINTS / 2; }
uint32_t SP_GetPeakF(void)    { return spPeakFDisp    ? spPeakFDisp    : spPeakF; }
uint16_t SP_GetPeakRssi(void) { return spPeakRssiDisp ? spPeakRssiDisp : spPeakRssi; }

void SP_RenderMarker(uint8_t mx, VMinMax v) {
  if (mx >= filledPoints) return;
  uint8_t barH = dbm2Y(Rssi2DBm(rssiHistory[mx]), v);
  uint8_t top  = S_BOTTOM - barH;

  for (uint8_t y = SPECTRUM_Y + 3; y < top; y += 2)
    PutPixel(mx, y, C_INVERT);

  FillRect(mx - 2, SPECTRUM_Y,     5, 1, C_INVERT);
  FillRect(mx - 1, SPECTRUM_Y + 1, 3, 1, C_INVERT);
  PutPixel(mx,     SPECTRUM_Y + 2,    C_INVERT);
}

// сетка 10 дБ/дел, как TinySA
void SP_RenderDbmGrid(VMinMax v, int8_t stepDbm) {
  int16_t dbMin = Rssi2DBm(v.vMin);
  int16_t dbMax = Rssi2DBm(v.vMax);
  for (int16_t dbm = (dbMin / stepDbm) * stepDbm;
       dbm <= dbMax; dbm += stepDbm) {
    if (dbm < dbMin) continue;
    uint8_t py = S_BOTTOM - dbm2Y(dbm, v);
    for (uint8_t xi = 0; xi < MAX_POINTS; xi += 4)
      DrawHLine(xi, py, 2, C_FILL);
    PrintSmallEx(0, py - 1, POS_L, C_FILL, "%d", dbm);
  }
}

// ────────────────────────────────────────────────────────────────────

void SP_Render(const Band *p, VMinMax v) {
  if (p) UI_DrawTicks(S_BOTTOM, p);
  DrawHLine(0, S_BOTTOM, MAX_POINTS, C_FILL);

  if (graphMeasurement == GRAPH_RSSI) {
    for (uint8_t i = 0; i < filledPoints; ++i) {
      uint8_t yVal = dbm2Y(Rssi2DBm(rssiHistory[i]), v);
      DrawVLine(i, S_BOTTOM - yVal, yVal, C_FILL);
    }
  } else {
    VMinMax rv = SP_GetGraphMinMax();
    for (uint8_t i = 0; i < filledPoints; ++i) {
      uint16_t val = (graphMeasurement == GRAPH_NOISE)
                     ? noiseHistory[i] : glitchHistory[i];
      uint8_t yVal = ConvertDomain(val, rv.vMin, rv.vMax, 0, SPECTRUM_H);
      DrawVLine(i, S_BOTTOM - yVal, yVal, C_FILL);
    }
  }
}

// ────────────────────────────────────────────────────────────────────

void SP_RenderArrow(uint32_t f) {
  uint8_t cx = SP_F2X(f);
  DrawVLine(cx, SPECTRUM_Y + SPECTRUM_H + 1, 1, C_FILL);
  FillRect(cx - 1, SPECTRUM_Y + SPECTRUM_H + 2, 3, 1, C_FILL);
  FillRect(cx - 2, SPECTRUM_Y + SPECTRUM_H + 3, 5, 1, C_FILL);
}

void SP_RenderRssi(uint16_t rssi, char *text, bool top, VMinMax v) {
  uint8_t yVal = dbm2Y(Rssi2DBm(rssi), v);
  DrawHLine(0, S_BOTTOM - yVal, filledPoints, C_FILL);
  PrintSmallEx(0, S_BOTTOM - yVal + (top ? -2 : 6), POS_L, C_FILL, "%s %d",
               text, Rssi2DBm(rssi));
}

void SP_RenderLine(uint16_t rssi, VMinMax v) {
  uint8_t yVal = dbm2Y(Rssi2DBm(rssi), v);
  DrawHLine(0, S_BOTTOM - yVal, filledPoints, C_INVERT);
}

void SP_RenderPoint(Measurement *m, uint8_t i, uint8_t n, Band *b, VMinMax r,
                    Color c) {
  uint8_t yVal = dbm2Y(Rssi2DBm(m->rssi), r);
  PutPixel(i, S_BOTTOM - yVal, c);
}

// ────────────────────────────────────────────────────────────────────

uint16_t SP_GetNoiseFloor(void) {
  if (filledPoints < 10) return 0;
  uint16_t temp[MAX_POINTS];
  memcpy(temp, rssiHistory, sizeof(uint16_t) * filledPoints);
  for (uint8_t i = 0; i < filledPoints - 1; i++)
    for (uint8_t j = i + 1; j < filledPoints; j++)
      if (temp[i] > temp[j]) {
        uint16_t t = temp[i]; temp[i] = temp[j]; temp[j] = t;
      }
  return temp[filledPoints / 4];
}

uint16_t SP_GetRssiMax(void)          { return Max(rssiHistory, filledPoints); }
uint16_t SP_GetPointRSSI(uint8_t i)   { return rssiHistory[i]; }
uint16_t SP_GetPointNoise(uint8_t i)  { return noiseHistory[i]; }
uint16_t SP_GetPointGlitch(uint8_t i) { return glitchHistory[i]; }
uint16_t SP_GetLastGraphValue(void)   { return rssiHistory[MAX_POINTS - 1]; }

// ────────────────────────────────────────────────────────────────────

void SP_RenderGraph(uint16_t min, uint16_t max) {
  const VMinMax v = {.vMin = min, .vMax = max};
  S_BOTTOM = SPECTRUM_Y + SPECTRUM_H;
  FillRect(0, SPECTRUM_Y, LCD_WIDTH, SPECTRUM_H, C_CLEAR);
  uint8_t oVal = ConvertDomain(rssiHistory[0], v.vMin, v.vMax, 0, SPECTRUM_H);
  const uint8_t centerY = SPECTRUM_Y + SPECTRUM_H / 2;
  if (graphMeasurement == GRAPH_TX) {
    for (uint8_t i = 0; i < MAX_POINTS; ++i) {
      uint8_t amp = ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H / 2);
      DrawVLine(i, centerY - amp, amp * 2 + 1, C_FILL);
    }
  } else {
    for (uint8_t i = 1; i < MAX_POINTS; ++i) {
      uint8_t yVal = ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H);
      DrawLine(i - 1, S_BOTTOM - oVal, i, S_BOTTOM - yVal, C_FILL);
      oVal = yVal;
    }
  }
  DrawHLine(0, SPECTRUM_Y, LCD_WIDTH, C_FILL);
  DrawHLine(0, S_BOTTOM,   LCD_WIDTH, C_FILL);
  for (uint8_t xi = 0; xi < LCD_WIDTH; xi += 4)
    DrawHLine(xi, centerY, 2, C_FILL);
}

void SP_NextGraphUnit(bool next) {
  graphMeasurement = IncDecU(graphMeasurement, 0, GRAPH_COUNT, next);
}

void SP_AddGraphPoint(const Measurement *msm) {
  uint16_t v = msm->rssi;
  switch (graphMeasurement) {
  case GRAPH_NOISE:  v = msm->noise;                 break;
  case GRAPH_GLITCH: v = msm->glitch;                break;
  case GRAPH_SNR:    v = msm->snr;                   break;
  case GRAPH_APRS:   v = BOARD_ADC_GetAPRS();        break;
  case GRAPH_TX:     v = BK4819_GetVoiceAmplitude();  break;
  default:           break;
  }
  rssiHistory[MAX_POINTS - 1] = v;
  filledPoints = MAX_POINTS;
}

// ────────────────────────────────────────────────────────────────────

static void shiftEx(uint16_t *history, uint16_t n, int16_t shift) {
  if (shift == 0) return;
  if (shift > 0) {
    while (shift-- > 0) {
      for (int16_t i = n - 2; i >= 0; --i) history[i + 1] = history[i];
      history[0] = 0;
    }
  } else {
    while (shift++ < 0) {
      for (int16_t i = 0; i < n - 1; ++i) history[i] = history[i + 1];
      history[MAX_POINTS - 1] = 0;
    }
  }
}

void SP_Shift(int16_t n)      { shiftEx(rssiHistory, MAX_POINTS, n); }
void SP_ShiftGraph(int16_t n) { shiftEx(rssiHistory, MAX_POINTS, n); }

// ────────────────────────────────────────────────────────────────────

static uint8_t curX = MAX_POINTS / 2;
static uint8_t curSbWidth = 16;

void CUR_Render(void) {
  for (uint8_t y = SPECTRUM_Y + 10; y < S_BOTTOM; y += 4) {
    DrawVLine(curX - curSbWidth, y, 2, C_INVERT);
    DrawVLine(curX + curSbWidth, y, 2, C_INVERT);
  }
  for (uint8_t y = SPECTRUM_Y + 10; y < S_BOTTOM; y += 2)
    DrawVLine(curX, y, 1, C_INVERT);
}

bool CUR_Move(bool up) {
  if (up)  { if (curX + curSbWidth < MAX_POINTS - 4) { curX += 4; return true; } }
  else     { if (curX - curSbWidth >= 4)              { curX -= 4; return true; } }
  return false;
}

bool CUR_Size(bool up) {
  if (up) {
    if (curX + curSbWidth < MAX_POINTS - 1 && curX - curSbWidth > 0) { curSbWidth++; return true; }
  } else {
    if (curSbWidth > 1) { curSbWidth--; return true; }
  }
  return false;
}

Band CUR_GetRange(Band *p, uint32_t step) {
  Band r = *p;
  r.start = RoundToStep(SP_X2F(curX - curSbWidth), step);
  r.end   = RoundToStep(SP_X2F(curX + curSbWidth), step);
  return r;
}

uint32_t CUR_GetCenterF(uint32_t step) {
  return RoundToStep(SP_X2F(curX), step);
}

void CUR_Reset(void) {
  curX       = MAX_POINTS / 2;
  curSbWidth = 16;
}

void CUR_SetPosByFreq(uint32_t f) {
  curX = SP_F2X(f);
}
