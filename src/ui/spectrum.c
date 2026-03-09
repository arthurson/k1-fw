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

static uint8_t S_BOTTOM;

static uint16_t rssiHistory[MAX_POINTS] = {0};

static uint8_t x = 0;
static uint8_t ox = UINT8_MAX;
static uint8_t filledPoints;

static Band *range;
static uint16_t step;

static void drawTicks(uint8_t y, uint32_t fs, uint32_t fe, uint32_t div,
                      uint8_t h) {
  for (uint32_t f = fs - (fs % div) + div; f < fe; f += div) {
    DrawVLine(SP_F2X(f), y, h, C_FILL);
  }
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

static uint8_t visited[MAX_POINTS / 8 + 1] = {0};

void SP_ResetHistory(void) {
  filledPoints = 0;
  memset(rssiHistory, 0, sizeof(rssiHistory));
  memset(visited, 0, sizeof(visited));
}

void SP_Begin(void) {
  x = 0;
  ox = UINT8_MAX;
  memset(visited, 0, sizeof(visited));
}

void SP_Init(Band *b) {
  S_BOTTOM = SPECTRUM_Y + SPECTRUM_H;
  range = b;
  step = StepFrequencyTable[b->step];
  SP_ResetHistory();
  SP_Begin();
}

/* uint8_t SP_F2X(uint32_t f) {
  return ConvertDomainF(f, range->rxF, range->txF, 0, MAX_POINTS - 1);
} */

/* uint32_t SP_X2F(uint8_t x) {
  return ConvertDomainF(x, 0, MAX_POINTS - 1, range->rxF, range->txF);
} */

uint8_t SP_F2X(uint32_t f) {
  if (f <= range->start)
    return 0;
  if (f >= range->end)
    return MAX_POINTS - 1;

  uint32_t delta = f - range->start;
  uint32_t aRange = range->end - range->start;

  return (uint8_t)((delta * (MAX_POINTS - 1) + aRange / 2) / aRange);
}

uint32_t SP_X2F(uint8_t x) {
  // Защита от выхода за массив (хотя uint8_t x вряд ли превысит, если
  // MAX_POINTS > 255)
  if (x >= MAX_POINTS)
    x = MAX_POINTS - 1;

  // Считаем шаг частоты на один пиксель
  // Лучше вынести расчет step в место, где меняется range, чтобы не делить
  // каждый раз!
  uint32_t step = (range->end - range->start) / (MAX_POINTS - 1);

  return range->start + (x * step);
}

#define _MIN(a, b) (((a) < (b)) ? (a) : (b))
#define _MAX(a, b) (((a) > (b)) ? (a) : (b))
void SP_AddPoint(const Measurement *msm) {
  // Находим центральный пиксель для этой частоты
  uint8_t xc = SP_F2X(msm->f);

  // Определяем границы зоны для этой точки
  // Для простоты: первая точка - левая половина, последняя - правая половина,
  // средние точки - центрированы между соседями

  static uint8_t prev_xc = 0;
  int16_t ixs, ixe; // Signed для избежания underflow

  uint8_t next_xc =
      (msm->f + step > range->end) ? (MAX_POINTS - 1) : SP_F2X(msm->f + step);

  if (msm->f == range->start) {
    // Первая точка: от начала до середины до следующей точки
    ixs = 0;
    ixe = xc + (next_xc - xc) / 2;
  } else if (msm->f + step > range->end) {
    // Последняя точка: от середины от предыдущей до конца
    ixs = prev_xc + (xc - prev_xc) / 2;
    ixe = MAX_POINTS - 1;
  } else {
    // Средняя точка: между серединами соседних интервалов
    int16_t prev_mid = prev_xc + (xc - prev_xc) / 2;
    int16_t next_mid = xc + (next_xc - xc) / 2;

    ixs = prev_mid;
    ixe = next_mid - 1; // Здесь может быть underflow
  }

  // Swap если нужно
  if (ixs > ixe) {
    int16_t temp = ixs;
    ixs = ixe;
    ixe = temp;
  }

  // Клип границы
  ixs = _MAX(0, ixs);
  ixe = _MIN(MAX_POINTS - 1, ixe);

  for (uint8_t x = (uint8_t)ixs; x <= (uint8_t)ixe; ++x) {
    uint8_t byte = x / 8;
    uint8_t bit = x % 8;

    if ((visited[byte] & (1 << bit)) == 0) {
      // первый раз попали в этот x в текущем скане
      rssiHistory[x] = msm->rssi;
      visited[byte] |= (1 << bit);
    } else {
      // уже попадали → обновляем максимум
      if (msm->rssi > rssiHistory[x]) {
        rssiHistory[x] = msm->rssi;
      }
    }

    if (x + 1 > filledPoints)
      filledPoints = x + 1;
  }

  prev_xc = xc;

  // Заполняем всю зону
  /* for (uint8_t x = (uint8_t)ixs; x <= (uint8_t)ixe; ++x) {
    if (ox != x) {
      ox = x;
      rssiHistory[x] = 0; // Удалите эту строку, если хотите брать max от
      // предыдущих сканов (не очищать бар, а обновлять пик)
    }
    if (msm->rssi > rssiHistory[x]) {
      rssiHistory[x] = msm->rssi;
    }
  }

  if ((uint8_t)ixe >= filledPoints) {
    filledPoints = (uint8_t)ixe + 1;
  }

  prev_xc = xc; */
}

static uint16_t MinRSSI(const uint16_t *array, size_t n) {
  if (array == NULL || n == 0) {
    return 0;
  }
  uint16_t min = array[0];
  for (size_t i = 1; i < n; ++i) {
    if (array[i] && array[i] < min) {
      min = array[i];
    }
  }
  return min;
}

VMinMax SP_GetMinMax() {
  if (filledPoints == 0) {
    return (VMinMax){.vMin = 50, .vMax = 150}; // fallback
  }

  // 1. Находим текущие характеристики за весь скан (или последние N точек)
  uint16_t rssiMax = Max(rssiHistory, filledPoints);
  uint16_t noiseFloor =
      SP_GetNoiseFloor(); // уже есть — медиана или std-подобная оценка
  uint16_t rssiMin = MinRSSI(rssiHistory, filledPoints);

  // 2. Более устойчивая оценка шумового пола (важно для слабых сигналов)
  uint16_t reliableFloor = noiseFloor;
  if (noiseFloor < 40 || noiseFloor > rssiMax - 10) {
    reliableFloor = rssiMin + (rssiMax - rssiMin) / 10; // защита от выбросов
  }

  // 3. Целевые границы с запасом
  uint16_t targetMin =
      reliableFloor - 12; // чуть ниже пола — чтобы видеть слабые сигналы
  uint16_t targetMax = rssiMax + 18; // запас сверху 18–25 дБ

  // 4. Минимальный динамический диапазон (чтобы не было слишком узкого окна)
  uint16_t minSpan = 50; // минимум 50 единиц (~25–30 дБ)
  if (targetMax - targetMin < minSpan) {
    targetMax = targetMin + minSpan;
  }

  // 5. Максимальный динамический диапазон (не растягивать слишком сильно)
  uint16_t maxSpan = 120; // ~60–70 дБ — комфортно для большинства дисплеев
  if (targetMax - targetMin > maxSpan) {
    targetMax = targetMin + maxSpan;
  }

  // 6. Плавность: применяем лёгкое сглаживание (exponential moving average)
  //    или гистерезис — изменения применяются только если разница > порога
  static uint16_t prevMin = 0;
  static uint16_t prevMax = 0;

  if (prevMin == 0 || prevMax == 0) { // первый раз
    prevMin = targetMin;
    prevMax = targetMax;
  }

#define HYST 8 // гистерезис в единицах RSSI (~4–5 дБ)
#define ALPHA 0.65 // коэффициент сглаживания (0.6–0.8 — хороший компромисс)

  int16_t diffMin = (int16_t)targetMin - (int16_t)prevMin;
  int16_t diffMax = (int16_t)targetMax - (int16_t)prevMax;

  if (ABS(diffMin) > HYST) {
    prevMin = (uint16_t)(prevMin * (1.0 - ALPHA) + targetMin * ALPHA + 0.5);
  }
  if (ABS(diffMax) > HYST) {
    prevMax = (uint16_t)(prevMax * (1.0 - ALPHA) + targetMax * ALPHA + 0.5);
  }

  return (VMinMax){.vMin = prevMin, .vMax = prevMax};
}

void SP_Render(const Band *p, VMinMax v) {
  if (p) {
    UI_DrawTicks(S_BOTTOM, p);
  }

  DrawHLine(0, S_BOTTOM, MAX_POINTS, C_FILL);

  for (uint8_t i = 0; i < filledPoints; ++i) {
    uint8_t yVal = ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H);
    DrawVLine(i, S_BOTTOM - yVal, yVal, C_FILL);
  }
}

/* void SP_Render(const Band *p, VMinMax v) {
  if (p) {
    UI_DrawTicks(S_BOTTOM, p);
  }

  DrawHLine(0, S_BOTTOM, MAX_POINTS, C_FILL);

  // Median filter для 3 точек (сохраняет пики лучше)
  uint16_t smoothed[MAX_POINTS];
  for (uint8_t i = 0; i < filledPoints; ++i) {
    if (i == 0 || i == filledPoints - 1) {
      smoothed[i] = rssiHistory[i]; // Края без изменений
    } else {
      uint16_t vals[3] = {rssiHistory[i - 1], rssiHistory[i],
                          rssiHistory[i + 1]};
      // Сортировка для median (простая для 3 элементов)
      if (vals[0] > vals[1]) {
        uint16_t t = vals[0];
        vals[0] = vals[1];
        vals[1] = t;
      }
      if (vals[1] > vals[2]) {
        uint16_t t = vals[1];
        vals[1] = vals[2];
        vals[2] = t;
      }
      if (vals[0] > vals[1]) {
        uint16_t t = vals[0];
        vals[0] = vals[1];
        vals[1] = t;
      }
      smoothed[i] = vals[1]; // Median
    }
  }

  for (uint8_t i = 0; i < filledPoints; ++i) {
    uint8_t yVal = ConvertDomain(smoothed[i], v.vMin, v.vMax, 0, SPECTRUM_H);
    DrawVLine(i, S_BOTTOM - yVal, yVal, C_FILL);
  }
} */

/* void SP_Render(const Band *p, VMinMax v) {
  if (p) {
    UI_DrawTicks(S_BOTTOM, p);
  }

  FillRect(0, SPECTRUM_Y, MAX_POINTS, SPECTRUM_H, C_CLEAR);
  DrawHLine(0, S_BOTTOM, MAX_POINTS, C_FILL);

  if (filledPoints > 0) {
    uint8_t oY = ConvertDomain(rssiHistory[0], v.vMin, v.vMax, 0, SPECTRUM_H);
    for (uint8_t i = 1; i < filledPoints; ++i) {
      uint8_t yVal =
          ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H);
      DrawLine(i - 1, S_BOTTOM - oY, i, S_BOTTOM - yVal, C_FILL);
      oY = yVal;
    }
  }
} */

/* void SP_Render(const Band *p, VMinMax v) {
  if (p) {
    UI_DrawTicks(S_BOTTOM, p);
  }

  // Очистка и базовые линии (грид для красоты)
  FillRect(0, SPECTRUM_Y, MAX_POINTS, SPECTRUM_H,
           C_CLEAR); // Полная очистка для свежести
  DrawHLine(0, S_BOTTOM, MAX_POINTS, C_FILL); // Нижняя линия

  // Адаптивное сглаживание (только если шаг широкий)
  bool smooth = (step > ((range->txF - range->rxF) / (MAX_POINTS - 1)) * 2);
  uint16_t values[MAX_POINTS];
  if (smooth) {
    // Median filter для сохранения пиков
    for (uint8_t i = 0; i < filledPoints; ++i) {
      if (i == 0 || i == filledPoints - 1) {
        values[i] = rssiHistory[i];
      } else {
        uint16_t vals[3] = {rssiHistory[i - 1], rssiHistory[i],
                            rssiHistory[i + 1]};
        // Простая сортировка для median
        if (vals[0] > vals[1]) {
          uint16_t t = vals[0];
          vals[0] = vals[1];
          vals[1] = t;
        }
        if (vals[1] > vals[2]) {
          uint16_t t = vals[1];
          vals[1] = vals[2];
          vals[2] = t;
        }
        if (vals[0] > vals[1]) {
          uint16_t t = vals[0];
          vals[0] = vals[1];
          vals[1] = t;
        }
        values[i] = vals[1];
      }
    }
  } else {
    // Без сглаживания для узких пиков
    for (uint8_t i = 0; i < filledPoints; ++i) {
      values[i] = rssiHistory[i];
    }
  }

  // Рисуй бары (VLine)
  for (uint8_t i = 0; i < filledPoints; ++i) {
    uint8_t yVal = ConvertDomain(values[i], v.vMin, v.vMax, 0, SPECTRUM_H);
    DrawVLine(i, S_BOTTOM - yVal, yVal, C_FILL);
  }
} */

void SP_RenderArrow(uint32_t f) {
  uint8_t cx = SP_F2X(f);
  DrawVLine(cx, SPECTRUM_Y + SPECTRUM_H + 1, 1, C_FILL);
  FillRect(cx - 1, SPECTRUM_Y + SPECTRUM_H + 2, 3, 1, C_FILL);
  FillRect(cx - 2, SPECTRUM_Y + SPECTRUM_H + 3, 5, 1, C_FILL);
}

void SP_RenderRssi(uint16_t rssi, char *text, bool top, VMinMax v) {
  uint8_t yVal = ConvertDomain(rssi, v.vMin, v.vMax, 0, SPECTRUM_H);
  DrawHLine(0, S_BOTTOM - yVal, filledPoints, C_FILL);
  PrintSmallEx(0, S_BOTTOM - yVal + (top ? -2 : 6), POS_L, C_FILL, "%s %d",
               text, Rssi2DBm(rssi));
}

void SP_RenderLine(uint16_t rssi, VMinMax v) {
  uint8_t yVal = ConvertDomain(rssi, v.vMin, v.vMax, 0, SPECTRUM_H);
  DrawHLine(0, S_BOTTOM - yVal, filledPoints, C_FILL);
}
void SP_RenderPoint(Measurement *m, uint8_t i, uint8_t n, Band *b, VMinMax r,
                    Color c) {
  uint8_t yVal = ConvertDomain(m->rssi, r.vMin, r.vMax, 0, SPECTRUM_H);
  PutPixel(i, S_BOTTOM - yVal, c);
}

// uint16_t SP_GetNoiseFloor() { return Std(rssiHistory, filledPoints); }
uint16_t SP_GetNoiseFloor() {
  if (filledPoints < 10)
    return 0;

  uint16_t temp[MAX_POINTS];
  memcpy(temp, rssiHistory, sizeof(uint16_t) * filledPoints);
  // Простая сортировка (можно bubble или qsort)
  for (uint8_t i = 0; i < filledPoints - 1; i++)
    for (uint8_t j = i + 1; j < filledPoints; j++)
      if (temp[i] > temp[j]) {
        uint16_t t = temp[i];
        temp[i] = temp[j];
        temp[j] = t;
      }

  // Берём ~25% перцентиль (шум без пиков)
  return temp[filledPoints / 4];
}
uint16_t SP_GetRssiMax() { return Max(rssiHistory, filledPoints); }

uint16_t SP_GetPointRSSI(uint8_t i) { return rssiHistory[i]; }
uint16_t SP_GetLastGraphValue() { return rssiHistory[MAX_POINTS - 1]; }

void SP_RenderGraph(uint16_t min, uint16_t max) {
  const VMinMax v = {
      .vMin = min,
      .vMax = max,
  };
  S_BOTTOM = SPECTRUM_Y + SPECTRUM_H; // TODO: mv to separate function

  FillRect(0, SPECTRUM_Y, LCD_WIDTH, SPECTRUM_H, C_CLEAR);

  uint8_t oVal = ConvertDomain(rssiHistory[0], v.vMin, v.vMax, 0, SPECTRUM_H);

  const uint8_t centerY = SPECTRUM_Y + SPECTRUM_H / 2;
  if (graphMeasurement == GRAPH_TX) {

    for (uint8_t i = 0; i < MAX_POINTS; ++i) {
      /* Амплитуда: половина высоты области → бар вверх и вниз от центра */
      uint8_t amp =
          ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H / 2);

      /* Заливка от (centerY - amp) до (centerY + amp) */
      DrawVLine(i, centerY - amp, amp * 2 + 1, C_FILL);
    }
  } else {
    for (uint8_t i = 1; i < MAX_POINTS; ++i) {
      uint8_t yVal =
          ConvertDomain(rssiHistory[i], v.vMin, v.vMax, 0, SPECTRUM_H);
      DrawLine(i - 1, S_BOTTOM - oVal, i, S_BOTTOM - yVal, C_FILL);
      oVal = yVal;
    }
  }
  DrawHLine(0, SPECTRUM_Y, LCD_WIDTH, C_FILL);
  DrawHLine(0, S_BOTTOM, LCD_WIDTH, C_FILL);

  for (uint8_t x = 0; x < LCD_WIDTH; x += 4) {
    DrawHLine(x, centerY, 2, C_FILL);
  }
}

void SP_NextGraphUnit(bool next) {
  graphMeasurement = IncDecU(graphMeasurement, 0, GRAPH_COUNT, next);
}

void SP_AddGraphPoint(const Measurement *msm) {
  uint16_t v = msm->rssi;

  switch (graphMeasurement) {
  case GRAPH_NOISE:
    v = msm->noise;
    break;
  case GRAPH_GLITCH:
    v = msm->glitch;
    break;
  case GRAPH_SNR:
    v = msm->snr;
    break;
  case GRAPH_APRS:
    v = BOARD_ADC_GetAPRS();
    break;
  case GRAPH_TX:
    v = BK4819_GetVoiceAmplitude();
    break;
  case GRAPH_RSSI:
  case GRAPH_COUNT:
    break;
  }

  rssiHistory[MAX_POINTS - 1] = v;
  filledPoints = MAX_POINTS;
}

static void shiftEx(uint16_t *history, uint16_t n, int16_t shift) {
  if (shift == 0) {
    return;
  }
  if (shift > 0) {
    while (shift-- > 0) {
      for (int16_t i = n - 2; i >= 0; --i) {
        history[i + 1] = history[i];
      }
      history[0] = 0;
    }
  } else {
    while (shift++ < 0) {
      for (int16_t i = 0; i < n - 1; ++i) {
        history[i] = history[i + 1];
      }
      history[MAX_POINTS - 1] = 0;
    }
  }
}

void SP_Shift(int16_t n) { shiftEx(rssiHistory, MAX_POINTS, n); }
void SP_ShiftGraph(int16_t n) { shiftEx(rssiHistory, MAX_POINTS, n); }

static uint8_t curX = MAX_POINTS / 2;
static uint8_t curSbWidth = 16;

void CUR_Render() {
  for (uint8_t y = SPECTRUM_Y + 10; y < S_BOTTOM; y += 4) {
    DrawVLine(curX - curSbWidth, y, 2, C_INVERT);
    DrawVLine(curX + curSbWidth, y, 2, C_INVERT);
  }
  for (uint8_t y = SPECTRUM_Y + 10; y < S_BOTTOM; y += 2) {
    DrawVLine(curX, y, 1, C_INVERT);
  }
}

bool CUR_Move(bool up) {
  if (up) {
    if (curX + curSbWidth < MAX_POINTS - 4) {
      curX += 4;
      return true;
    }
  } else {
    if (curX - curSbWidth >= 4) {
      curX -= 4;
      return true;
    }
  }
  return false;
}

bool CUR_Size(bool up) {
  if (up) {
    if (curX + curSbWidth < MAX_POINTS - 1 && curX - curSbWidth > 0) {
      curSbWidth++;
      return true;
    }
  } else {
    if (curSbWidth > 1) {
      curSbWidth--;
      return true;
    }
  }
  return false;
}

Band CUR_GetRange(Band *p, uint32_t step) {
  Band range = *p;
  range.start = SP_X2F(curX - curSbWidth);
  range.end = SP_X2F(curX + curSbWidth),
  range.start = RoundToStep(range.start, step);
  range.end = RoundToStep(range.end, step);
  return range;
}

uint32_t CUR_GetCenterF(uint32_t step) {
  return RoundToStep(SP_X2F(curX), step);
}

void CUR_Reset() {
  curX = MAX_POINTS / 2;
  curSbWidth = 16;
}
