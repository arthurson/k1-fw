#include "analyser.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/analysermenu.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../helper/storage.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "../ui/toast.h"
#include "apps.h"
#include <stdbool.h>
#include <string.h>

// ── Constants ──────────────────────────────────────────────────────────────

#define ANALYSER_SETTINGS_MAGIC 0xA51Du // bump при смене структуры
#define SQ_LEVEL_INVALID 0xFF
#define CURSOR_INFO_TIMEOUT_MS 2000
#define SETTINGS_SAVE_DEBOUNCE_MS 1000
#define DELAY_BLOCKING_MAX_US 10000 // ≤10мс — блокирующая задержка
#define DELAY_DEFAULT_US 2200
#define DELAY_MAX_US 90000
#define DEFAULT_RANGE_START 43307500u

// dBm limits
#define DBM_MIN_LO -140
#define DBM_MIN_HI 9
#define DBM_MAX_LO -139
#define DBM_MAX_HI 10

// Waterfall geometry
#define WF_H 16
#define WF_GAP 4
#define SPECTRUM_H_FULL 44
#define SPECTRUM_H_SPLIT (SPECTRUM_H_FULL - WF_H - WF_GAP)

// ── Persisted settings ─────────────────────────────────────────────────────

typedef struct {
  uint16_t magic;
  int16_t dbMin;
  int16_t dbMax;
  uint32_t delay;
  uint32_t lastRangeStart;
  uint32_t lastRangeEnd;
  uint8_t waterfallEnabled;
  uint8_t mode;
  uint8_t _pad[2];
} AnalyserSettings;

static AnalyserSettings aSettings = {
    .magic = ANALYSER_SETTINGS_MAGIC,
    .dbMin = -120,
    .dbMax = -20,
    .delay = DELAY_DEFAULT_US,
    .lastRangeStart = 0,
    .lastRangeEnd = 0,
    .waterfallEnabled = 0,
    .mode = 0,
};

static uint32_t analyserSaveTime;
static bool settingsDirty;

static void analyserSettingsLoad(void) {
  AnalyserSettings loaded = {0};
  STORAGE_LOAD("analyser.set", 0, &loaded);
  if (loaded.magic == ANALYSER_SETTINGS_MAGIC) {
    aSettings = loaded;
  }
}

static void analyserSettingsSave(void) {
  aSettings.magic = ANALYSER_SETTINGS_MAGIC;
  STORAGE_SAVE("analyser.set", 0, &aSettings);
}

// Каждое изменение перезапускает таймер — настоящий debounce
static void markSettingsDirty(void) {
  settingsDirty = true;
  analyserSaveTime = Now() + SETTINGS_SAVE_DEBOUNCE_MS;
}

// ── Analyser state ─────────────────────────────────────────────────────────

static Band range;
static uint32_t targetF;
static uint32_t delay = DELAY_DEFAULT_US;
static bool still;
static bool listen;
static Measurement *msm;
static uint32_t cursorRangeTimeout = 0;

// Неблокирующее ожидание settle при delay > DELAY_BLOCKING_MAX_US
static uint32_t scanWaitUntil;
static bool scanAwaitingSettle;

// Маркеры A/B. Auto = копируется из peaks[0/1] каждый свип.
// Manual = зафиксирован пользователем через long KEY_5, не обновляется.
typedef struct {
  uint32_t f;
  bool manual;
} Marker;
static Marker markerA;
static Marker markerB;

// Waterfall
static bool waterfallOn;
static uint8_t wfBuf[WF_H][LCD_WIDTH]; // [row][x] = rssi clamped to 0..255
static uint8_t wfHead; // текущая (пишущаяся) строка
static uint8_t
    wfFilled; // кол-во полностью заполненных старых строк (0..WF_H-1)
static uint8_t wfPrevXc; // trailing центр предыдущего бина (как spPrevXc)

// Squelch tuner
static SQL sq;
static uint8_t sqStp = 10;
static bool showSqTuner;

typedef enum {
  SQ_EDIT_RSSI,
  SQ_EDIT_NOISE,
  SQ_EDIT_GLITCH,
} SqEditParam;

static SqEditParam sqEditParam = SQ_EDIT_RSSI;
static uint8_t sqEditLevel;
static uint8_t lastSquelchLevel = SQ_LEVEL_INVALID;

// dBm tuner (редактирует верхнюю/нижнюю границу шкалы Y)
static bool showDbmTuner;

// Режимы анализатора (переключаются short KEY_SIDE2, сохраняются)
typedef enum {
  MODE_SCAN,        // S: спектр, без listen
  MODE_PEAKS,       // P: спектр + таблица пиков с R/N/G
  MODE_SCAN_LISTEN, // H: спектр + listen по шумодаву + loot controls
  MODE_COUNT,
} AnalyserMode;

static AnalyserMode analyserMode = MODE_SCAN;

// Top-N пиков в режиме PEAKS
#define PEAKS_N 3
#define PEAKS_MIN_GAP 8 // мин. расстояние между пиками, пикс

typedef struct {
  uint8_t x;
  uint16_t rssi;
} PeakEntry;

static PeakEntry peaks[PEAKS_N];
static uint8_t peaksFound;

// ── Delay helpers ──────────────────────────────────────────────────────────

static uint32_t adjustDelay(int32_t inc) {
  uint32_t step;
  if (delay < 3000) {
    step = 100;
  } else if (delay < 10000) {
    step = 1000;
  } else {
    step = 5000;
  }
  int32_t newDelay =
      (int32_t)delay + (inc > 0 ? (int32_t)step : -(int32_t)step);
  if (newDelay < 0)
    return 0;
  if (newDelay > DELAY_MAX_US)
    return DELAY_MAX_US;
  return (uint32_t)newDelay;
}

static void setDelayUs(uint32_t newDelay) {
  if (newDelay == delay)
    return;
  delay = newDelay;
  scanAwaitingSettle = false; // старый scanWaitUntil невалиден
  markSettingsDirty();
}

// ── Squelch helpers ────────────────────────────────────────────────────────

static void sqApplyThresholds(SquelchPreset p) {
  sq.ro = p.ro;
  sq.no = p.no;
  sq.go = p.go;
  sq.rc = sq.ro > SQ_HYSTERESIS ? sq.ro - SQ_HYSTERESIS : 0;
  sq.nc = sq.no + SQ_HYSTERESIS;
  sq.gc = sq.go + SQ_HYSTERESIS;
}

static void applySquelchPreset(void) {
  if (ctx->squelch.value != lastSquelchLevel) {
    lastSquelchLevel = ctx->squelch.value;
    sqEditLevel = ctx->squelch.value;
    sqApplyThresholds(GetSqlPreset(sqEditLevel, ctx->frequency));
  }
}

// ── Peaks ──────────────────────────────────────────────────────────────────

// Жадно ищем топ-N пиков в rssiHistory с минимальным gap между ними.
// Вызывать после завершения свипа, пока visited ещё содержит актуальные данные.
static void updatePeaks(void) {
  peaksFound = 0;
  bool used[LCD_WIDTH] = {0};

  for (uint8_t p = 0; p < PEAKS_N; p++) {
    uint16_t best = 0;
    uint8_t bestX = 0xFF;
    for (uint8_t x = 0; x < LCD_WIDTH; x++) {
      if (used[x])
        continue;
      uint16_t r = SP_GetPointRSSI(x);
      if (r > best) {
        best = r;
        bestX = x;
      }
    }
    if (bestX == 0xFF || best == 0)
      break;

    peaks[p].x = bestX;
    peaks[p].rssi = best;
    peaksFound++;

    uint8_t lo = (bestX > PEAKS_MIN_GAP) ? bestX - PEAKS_MIN_GAP : 0;
    uint8_t hi = (bestX + PEAKS_MIN_GAP < LCD_WIDTH) ? bestX + PEAKS_MIN_GAP
                                                     : LCD_WIDTH - 1;
    for (uint8_t x = lo; x <= hi; x++)
      used[x] = true;
  }
}

// ── Mode switching ─────────────────────────────────────────────────────────

// Нужна ли уменьшенная высота спектра (для waterfall / peaks / RSSI bar)
static bool needSplit(void) {
  return waterfallOn || analyserMode == MODE_PEAKS ||
         analyserMode == MODE_SCAN_LISTEN || still || listen;
}

// Пересчитать SPECTRUM_H под текущее состояние. SP_Init сбросит буфер —
// вызывать только когда состояние действительно изменилось.
static void refreshSpectrumH(void) {
  uint8_t newH = needSplit() ? SPECTRUM_H_SPLIT : SPECTRUM_H_FULL;
  if (newH == SPECTRUM_H)
    return;
  SPECTRUM_H = newH;
  SP_Init(&range);
}

// Обновить auto-маркеры на топ-2 пика (вызывается после updatePeaks)
static void updateMarkersFromPeaks(void) {
  if (!markerA.manual)
    markerA.f = (peaksFound > 0) ? SP_X2F(peaks[0].x) : 0;
  if (!markerB.manual)
    markerB.f = (peaksFound > 1) ? SP_X2F(peaks[1].x) : 0;
}

static void cycleMode(void) {
  analyserMode = (analyserMode + 1) % MODE_COUNT;
  peaksFound = 0;
  markSettingsDirty();
  refreshSpectrumH();
  gRedrawScreen = true;
}

// ── Markers ────────────────────────────────────────────────────────────────

// FINPUT callback для long KEY_5 в scan-режимах — ставит ручной маркер
// в первый auto-слот.
static void onMarkerFreq(uint32_t fs, uint32_t fe) {
  (void)fe;
  if (!markerA.manual) {
    markerA.f = fs;
    markerA.manual = true;
  } else if (!markerB.manual) {
    markerB.f = fs;
    markerB.manual = true;
  }
}

// FINPUT callback для long KEY_5 в STILL — переход на введённую частоту
static void onStillJumpFreq(uint32_t fs, uint32_t fe) {
  (void)fe;
  targetF = fs;
  msm->f = fs;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, fs, false);
  RADIO_ApplySettings(ctx);
}

// Long KEY_5 в scan-режимах: если оба маркера manual → сброс в auto.
// Иначе — FINPUT для нового manual маркера.
static void handleMarkerInput(void) {
  if (markerA.manual && markerB.manual) {
    markerA.manual = false;
    markerB.manual = false;
    gRedrawScreen = true;
    return;
  }
  FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
  FINPUT_Show(onMarkerFreq);
}

// ── Waterfall ──────────────────────────────────────────────────────────────

static void wfReset(void) {
  memset(wfBuf, 0, sizeof(wfBuf));
  wfHead = 0;
  wfFilled = 0;
  wfPrevXc = 0;
}

// Пишем измерение в текущую строку waterfall.
// Бар на частоте f занимает диапазон [ixs, ixe] — SP_F2X возвращает ЦЕНТР,
// границы считаются как полпути до соседних центров (как в SP_AddPoint).
// Внутри диапазона берём max, чтобы повторные измерения не затирали пик.
static void wfOnMeasure(void) {
  if (!waterfallOn)
    return;

  uint32_t f = msm->f;
  uint8_t xc = SP_F2X(f);
  if (xc == 0xFF)
    return;

  uint32_t stepHz = StepFrequencyTable[range.step];
  uint8_t next_xc =
      (f + stepHz > range.end) ? (LCD_WIDTH - 1) : SP_F2X(f + stepHz);

  int16_t ixs, ixe;
  if (f == range.start) {
    ixs = 0;
    ixe = xc + ((int16_t)next_xc - xc) / 2;
  } else if (f + stepHz > range.end) {
    ixs = wfPrevXc + ((int16_t)xc - wfPrevXc) / 2;
    ixe = LCD_WIDTH - 1;
  } else {
    ixs = wfPrevXc + ((int16_t)xc - wfPrevXc) / 2;
    ixe = xc + ((int16_t)next_xc - xc) / 2 - 1;
  }

  if (ixs > ixe) {
    int16_t t = ixs;
    ixs = ixe;
    ixe = t;
  }
  if (ixs < 0)
    ixs = 0;
  if (ixe > LCD_WIDTH - 1)
    ixe = LCD_WIDTH - 1;

  uint16_t r = msm->rssi;
  uint8_t rByte = (r > 255) ? 255 : (uint8_t)r;

  uint8_t *row = wfBuf[wfHead];
  for (uint8_t x = (uint8_t)ixs; x <= (uint8_t)ixe; x++) {
    if (rByte > row[x])
      row[x] = rByte;
  }

  wfPrevXc = xc;
}

// Свип завершён — сдвигаем голову, обнуляем новую строку
static void wfOnSweepEnd(void) {
  if (!waterfallOn)
    return;
  if (wfFilled < WF_H - 1)
    wfFilled++;
  wfHead = (wfHead + 1) % WF_H;
  memset(wfBuf[wfHead], 0, LCD_WIDTH);
  wfPrevXc = 0;
}

static void wfSetEnabled(bool on) {
  if (waterfallOn == on)
    return;
  waterfallOn = on;
  SPECTRUM_H = on ? SPECTRUM_H_SPLIT : SPECTRUM_H_FULL;
  wfReset();
  SP_Init(&range);
  markSettingsDirty();
  gRedrawScreen = true;
}

// 2x2 dither → 4 градации + выкл. Порог привязан к rowIdx (индекс данных),
// чтобы паттерн прокручивался вместе с waterfall и не мерцал.
static const uint8_t bayer2[2][2] = {
    {0, 2},
    {3, 1},
};

// Рендер: row=0 — текущая (частично заполненная), вниз — старшие.
// Сырой rssi в буфере нормализуется в 0..255 по vMin/vMax — при правке
// dBm-шкалы весь waterfall сразу переэкспонируется.
static void wfRender(VMinMax v) {
  if (!waterfallOn)
    return;
  int32_t span = (int32_t)v.vMax - (int32_t)v.vMin;
  if (span <= 0)
    return;

  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H + WF_GAP;
  uint8_t rows = wfFilled + 1;
  if (rows > WF_H)
    rows = WF_H;

  for (uint8_t r = 0; r < rows; r++) {
    uint8_t rowIdx = (wfHead + WF_H - r) % WF_H;
    uint8_t y = y0 + r;
    uint8_t by = rowIdx & 1;
    const uint8_t *line = wfBuf[rowIdx];
    for (uint8_t x = 0; x < LCD_WIDTH; x++) {
      uint8_t val = line[x];
      if (val == 0)
        continue;
      int32_t norm = ((int32_t)val - (int32_t)v.vMin) * 255 / span;
      if (norm <= 0)
        continue;
      uint8_t level = (norm >= 255) ? 4 : (uint8_t)(norm * 5 / 256);
      if (level > bayer2[by][x & 1]) {
        PutPixel(x, y, 1);
      }
    }
  }
}

// ── dBm tuner helpers ──────────────────────────────────────────────────────

static int16_t clampI16(int16_t v, int16_t lo, int16_t hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

// Применить новые min/max с clamp и гарантией min < max
static void applyDbmBounds(int16_t newMin, int16_t newMax, bool maxChanged) {
  newMin = clampI16(newMin, DBM_MIN_LO, DBM_MIN_HI);
  newMax = clampI16(newMax, DBM_MAX_LO, DBM_MAX_HI);
  if (newMax <= newMin) {
    // Двигаем тот, который пользователь НЕ крутил
    if (maxChanged)
      newMin = newMax - 1;
    else
      newMax = newMin + 1;
  }
  ANALYSERMENU_SetDbmMin(newMin);
  ANALYSERMENU_SetDbmMax(newMax);
  markSettingsDirty();
  gRedrawScreen = true;
}

// ── Save scheduler ─────────────────────────────────────────────────────────

void ANALYSER_UpdateSave(void) {
  if (!settingsDirty)
    return;
  if (Now() < analyserSaveTime)
    return;

  aSettings.dbMin = ANALYSERMENU_GetDbmMin();
  aSettings.dbMax = ANALYSERMENU_GetDbmMax();
  aSettings.delay = delay;
  aSettings.lastRangeStart = range.start;
  aSettings.lastRangeEnd = range.end;
  aSettings.waterfallEnabled = waterfallOn ? 1 : 0;
  aSettings.mode = (uint8_t)analyserMode;
  analyserSettingsSave();
  settingsDirty = false;
}

// ── Auto-scale dBm ─────────────────────────────────────────────────────────
// Двойной тап KEY_6: подогнать dBm-шкалу под min/max последнего свипа.

#define AUTOSCALE_MARGIN_DB 5
#define KEY6_DOUBLE_TAP_MS 300

static uint32_t key6PendingSingle; // время RELEASED первого тапа

static void autoScaleDbm(void) {
  uint16_t rMin = 0xFFFF;
  uint16_t rMax = 0;
  for (uint8_t x = 0; x < LCD_WIDTH; x++) {
    uint16_t r = SP_GetPointRSSI(x);
    if (r == 0)
      continue;
    if (r < rMin)
      rMin = r;
    if (r > rMax)
      rMax = r;
  }
  if (rMin == 0xFFFF || rMax == 0) {
    TOAST_Push("No data");
    return;
  }

  int16_t dbMin = Rssi2DBm(rMin) - AUTOSCALE_MARGIN_DB;
  int16_t dbMax = Rssi2DBm(rMax) + AUTOSCALE_MARGIN_DB;
  applyDbmBounds(dbMin, dbMax, true);
  TOAST_Push("Scale %d..%d", dbMin, dbMax);
}

// ── Auto-squelch ───────────────────────────────────────────────────────────
// Калибровка порогов по N свипам пустого эфира. Усреднение → margin.
// Запускается из sqTuner (KEY_5), сохраняется в VHF/UHF preset под
// текущий squelch level.

#define AUTO_SQ_SWEEPS 3
#define AUTO_SQ_RSSI_MARGIN 8
#define AUTO_SQ_NOISE_MARGIN 8
#define AUTO_SQ_GLITCH_MARGIN 10

static bool autoSqActive;
static uint8_t autoSqSweepCnt;
static uint32_t autoSqRssiSum;
static uint32_t autoSqNoiseSum;
static uint32_t autoSqGlitchSum;
static uint32_t autoSqSamples;

static void autoSqStart(void) {
  if (still) {
    TOAST_Push("AutoSQL: scan only");
    return;
  }
  autoSqActive = true;
  autoSqSweepCnt = 0;
  autoSqRssiSum = 0;
  autoSqNoiseSum = 0;
  autoSqGlitchSum = 0;
  autoSqSamples = 0;
  TOAST_Push("AutoSQL: calibrating");
}

static void autoSqAbort(void) {
  if (!autoSqActive)
    return;
  autoSqActive = false;
  TOAST_Push("AutoSQL: aborted");
}

// Вызывать в measure() при каждом замере
static void autoSqSample(void) {
  if (!autoSqActive)
    return;
  if (still) {
    autoSqAbort();
    return;
  }
  autoSqRssiSum += msm->rssi;
  autoSqNoiseSum += msm->noise;
  autoSqGlitchSum += msm->glitch;
  autoSqSamples++;
}

// Вызывать на конце каждого свипа
static void autoSqOnSweep(void) {
  if (!autoSqActive)
    return;
  if (++autoSqSweepCnt < AUTO_SQ_SWEEPS)
    return;
  autoSqActive = false;

  if (!autoSqSamples) {
    TOAST_Push("AutoSQL: no data");
    return;
  }

  uint16_t avgR = autoSqRssiSum / autoSqSamples;
  uint16_t avgN = autoSqNoiseSum / autoSqSamples;
  uint16_t avgG = autoSqGlitchSum / autoSqSamples;

  SquelchPreset p;
  uint32_t ro = (uint32_t)avgR + AUTO_SQ_RSSI_MARGIN;
  p.ro = (ro > 255) ? 255 : (uint8_t)ro;
  p.no = (avgN > AUTO_SQ_NOISE_MARGIN) ? (uint8_t)(avgN - AUTO_SQ_NOISE_MARGIN)
                                       : 0;
  p.go = (avgG > AUTO_SQ_GLITCH_MARGIN)
             ? (uint8_t)(avgG - AUTO_SQ_GLITCH_MARGIN)
             : 0;

  sqApplyThresholds(p);
  SQ_SavePreset(ctx->frequency >= SETTINGS_GetFilterBound() ? "/uhf.sq"
                                                            : "/vhf.sq",
                sqEditLevel, &p);
  TOAST_Push("SQL R%u N%u G%u", p.ro, p.no, p.go);
}

// ── Range helpers ──────────────────────────────────────────────────────────

static void setRange(uint32_t fs, uint32_t fe) {
  range.step = RADIO_GetParam(ctx, PARAM_STEP);
  range.start = fs;
  range.end = fe;
  msm->f = range.start;
  SP_Init(&range);
  BANDS_RangeClear();
  BANDS_RangePush(range);
  wfReset();
  scanAwaitingSettle = false;
  markSettingsDirty();
}

static void lockToPeak(void) {
  uint32_t f = SP_GetPeakF();
  if (!f)
    return;
  still = true;
  targetF = f;
  msm->f = f;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, false);
  RADIO_ApplySettings(ctx);
}

// ── Mode indicator ─────────────────────────────────────────────────────────

static char getModeChar(void) {
  if (showSqTuner)
    return 'Q';
  if (showDbmTuner)
    return 'D';
  if (listen)
    return 'L';
  if (still)
    return 'T';
  switch (analyserMode) {
  case MODE_PEAKS:
    return 'P';
  case MODE_SCAN_LISTEN:
    return 'H';
  default:
    return 'S';
  }
}

// ── Key handlers ───────────────────────────────────────────────────────────

// dBm tuner: 2/8 крутят min, 3/9 крутят max. Autorepeat через
// LONG_PRESSED_CONT.
static bool dbmTunerKey(KEY_Code_t key, Key_State_t state) {
  int16_t dbMin = ANALYSERMENU_GetDbmMin();
  int16_t dbMax = ANALYSERMENU_GetDbmMax();

  switch (key) {
  case KEY_2:
    applyDbmBounds(dbMin + 1, dbMax, false);
    return true;
  case KEY_8:
    applyDbmBounds(dbMin - 1, dbMax, false);
    return true;
  case KEY_3:
    applyDbmBounds(dbMin, dbMax + 1, true);
    return true;
  case KEY_9:
    applyDbmBounds(dbMin, dbMax - 1, true);
    return true;
  default:
    break;
  }
  return false;
}

static bool sqTunerKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_1:
  case KEY_7: {
    graphMeasurement = GRAPH_RSSI;
    sqEditParam = SQ_EDIT_RSSI;
    int32_t delta = (key == KEY_1) ? sqStp : -(int32_t)sqStp;
    sq.ro = AdjustU(sq.ro, 0, 255, delta);
    sq.rc = sq.ro > SQ_HYSTERESIS ? sq.ro - SQ_HYSTERESIS : 0;
    return true;
  }
  case KEY_2:
  case KEY_8: {
    graphMeasurement = GRAPH_NOISE;
    sqEditParam = SQ_EDIT_NOISE;
    int32_t delta = (key == KEY_2) ? sqStp : -(int32_t)sqStp;
    sq.no = AdjustU(sq.no, 0, 128, delta);
    sq.nc = sq.no + SQ_HYSTERESIS;
    return true;
  }
  case KEY_3:
  case KEY_9: {
    graphMeasurement = GRAPH_GLITCH;
    sqEditParam = SQ_EDIT_GLITCH;
    int32_t delta = (key == KEY_3) ? sqStp : -(int32_t)sqStp;
    sq.go = AdjustU(sq.go, 0, 255, delta);
    sq.gc = sq.go + SQ_HYSTERESIS;
    return true;
  }
  case KEY_4:
  case KEY_6:
    setDelayUs(adjustDelay(key == KEY_4 ? 1 : -1));
    return true;
  case KEY_5:
    autoSqStart();
    return true;
  case KEY_0:
    sqStp = (sqStp >= 100) ? 1 : sqStp * 10;
    return true;
  case KEY_MENU: {
    SquelchPreset preset = {.ro = sq.ro, .no = sq.no, .go = sq.go};
    SQ_SavePreset(ctx->frequency >= SETTINGS_GetFilterBound() ? "/uhf.sq"
                                                              : "/vhf.sq",
                  sqEditLevel, &preset);
    return true;
  }
  default:
    break;
  }
  return false;
}

static bool stillModeKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_UP:
  case KEY_DOWN: {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    int32_t delta = (key == KEY_UP) ? (int32_t)step : -(int32_t)step;
    if (gSettings.invertButtons)
      delta = -delta;
    targetF += delta;
    msm->f = targetF;
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
    RADIO_ApplySettings(ctx);
    return true;
  }
  case KEY_4:
  case KEY_6: {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    int32_t delta = (key == KEY_4) ? (int32_t)step : -(int32_t)step;
    targetF += delta;
    msm->f = targetF;
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
    RADIO_ApplySettings(ctx);
    return true;
  }
  case KEY_SIDE1:
    if (state == KEY_LONG_PRESSED || state == KEY_LONG_PRESSED_CONT) {
      LOOT_BlacklistLast();
    } else {
      gMonitorMode = !gMonitorMode;
    }
    return true;
  case KEY_SIDE2:
    LOOT_WhitelistLast();
    return true;
  default:
    break;
  }
  return false;
}

static bool analyzerModeKey(KEY_Code_t key, Key_State_t state) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  if (state == KEY_LONG_PRESSED) {
    switch (key) {}
  }
  switch (key) {
  case KEY_UP:
  case KEY_DOWN: {
    bool moved = CUR_Move((key == KEY_UP) ^ gSettings.invertButtons);
    cursorRangeTimeout = Now() + CURSOR_INFO_TIMEOUT_MS;

    if (!moved) {
      bool up = (key == KEY_UP) ^ gSettings.invertButtons;
      if (up) {
        // Правый край → сдвигаем вправо
        range.start += step;
        range.end += step;
        SP_ShiftGraph(-1);
      } else {
        // Левый край → сдвигаем влево с защитой от underflow
        if (range.start >= step) {
          range.start -= step;
          range.end -= step;
          SP_ShiftGraph(1);
        } else {
          range.start = 0;
          range.end = StepFrequencyTable[range.step] * LCD_WIDTH;
        }
      }
      if (range.end > BK4819_F_MAX) {
        range.end = BK4819_F_MAX;
        range.start = range.end - StepFrequencyTable[range.step] * LCD_WIDTH;
      }
      BANDS_RangePeek()->start = range.start;
      BANDS_RangePeek()->end = range.end;
      SP_Begin();
      wfReset();
      scanAwaitingSettle = false;
      gRedrawScreen = true;
      markSettingsDirty();
    }
    return true;
  }

  case KEY_1:
  case KEY_7:
    setDelayUs(adjustDelay(key == KEY_1 ? 1 : -1));
    return true;

  case KEY_2: { // zoom in по выделению курсора
    Band zoomed = CUR_GetRange(BANDS_RangePeek(), step);
    zoomed.step = RADIO_GetParam(ctx, PARAM_STEP);
    BANDS_RangePush(zoomed);
    range = *BANDS_RangePeek();
    msm->f = range.start;
    SP_Init(&range);
    CUR_Reset();
    wfReset();
    scanAwaitingSettle = false;
    markSettingsDirty();
    return true;
  }

  case KEY_8: // zoom out
    BANDS_RangePop();
    range = *BANDS_RangePeek();
    RADIO_SetParam(ctx, PARAM_STEP, range.step, true);
    msm->f = range.start;
    SP_Init(&range);
    CUR_Reset();
    wfReset();
    scanAwaitingSettle = false;
    markSettingsDirty();
    return true;

  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, false);
    range.step = RADIO_GetParam(ctx, PARAM_STEP);
    BANDS_RangePeek()->step = range.step;
    SP_Init(&range);
    return true;

  case KEY_4:
    if (state == KEY_LONG_PRESSED) {
      if (gLastActiveLoot) {
        still = true;
        listen = true;
        targetF = msm->f = gLastActiveLoot->f;
        RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
        RADIO_ApplySettings(ctx);
        refreshSpectrumH();
      }
    } else if (state == KEY_RELEASED) {
      still = !still;
      if (still) {
        listen = true;
        targetF = msm->f;
        RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
        RADIO_ApplySettings(ctx);
      } else {
        listen = false; // выход из still — явно выключаем listen
      }
      refreshSpectrumH();
    }
    return true;

  case KEY_6:
    if (state == KEY_LONG_PRESSED) {
      showDbmTuner = !showDbmTuner;
      if (showDbmTuner)
        showSqTuner = false;
      key6PendingSingle = 0;
      return true;
    }
    if (state == KEY_RELEASED) {
      uint32_t now = Now();
      if (key6PendingSingle &&
          (now - key6PendingSingle) < KEY6_DOUBLE_TAP_MS) {
        // Второй тап → autoscale
        autoScaleDbm();
        key6PendingSingle = 0;
      } else {
        // Первый тап — откладываем lockToPeak до истечения double-tap окна
        key6PendingSingle = now ? now : 1;
      }
    }
    return true;

  case KEY_SIDE1:
    if (state == KEY_LONG_PRESSED) {
      LOOT_WhitelistLast();
      return true;
    }
    LOOT_BlacklistLast();
    return true;
  case KEY_SIDE2:
    if (state == KEY_LONG_PRESSED) {
      // В SCAN_LISTEN waterfall не актуален (area занята loot/RSSI)
      if (analyserMode != MODE_SCAN_LISTEN) {
        wfSetEnabled(!waterfallOn);
      }
      return true;
    }
    if (state == KEY_RELEASED) {
      cycleMode();
    }
    return true;
  case KEY_STAR:
    APPS_run(APP_LOOTLIST);
    return true;
  default:
    break;
  }
  return false;
}

bool ANALYSER_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state))
    return true;

  if (key == KEY_0 && state == KEY_LONG_PRESSED) {
    ANALYSERMENU_Toggle();
    return true;
  }

  if (ANALYSERMENU_Key(key, state))
    return true;

  if (state == KEY_RELEASED) {
    if (key == KEY_EXIT) {
      if (listen) {
        listen = false;
        refreshSpectrumH();
        return true;
      }
      if (still) {
        still = false;
        refreshSpectrumH();
        return true;
      }
      if (showSqTuner) {
        showSqTuner = false;
        return true;
      }
      if (showDbmTuner) {
        showDbmTuner = false;
        return true;
      }
    }
    if (key == KEY_F) {
      showSqTuner = !showSqTuner;
      if (showSqTuner) {
        showDbmTuner = false;
        sqEditLevel = ctx->squelch.value;
        sqApplyThresholds(GetSqlPreset(sqEditLevel, ctx->frequency));
        sqEditParam = SQ_EDIT_RSSI;
      }
      return true;
    }
    if (key == KEY_5) {
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
      FINPUT_Show(setRange);
      return true;
    }
  }

  // Long KEY_5: в STILL — переход на частоту, в scan-режимах — marker input
  if (key == KEY_5 && state == KEY_LONG_PRESSED) {
    if (still) {
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
      FINPUT_Show(onStillJumpFreq);
    } else {
      handleMarkerInput();
    }
    return true;
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED ||
      state == KEY_LONG_PRESSED_CONT) {
    if (showDbmTuner && dbmTunerKey(key, state))
      return true;
    if (showSqTuner && sqTunerKey(key, state))
      return true;
    if (still && stillModeKey(key, state))
      return true;
    if (!still)
      return analyzerModeKey(key, state);
  }
  return false;
}

// ── Init / deinit ──────────────────────────────────────────────────────────

static bool isValidRange(uint32_t start, uint32_t end) {
  return start < end && end <= BK4819_F_MAX &&
         (end - start) >= StepFrequencyTable[0];
}

void ANALYSER_init(void) {
  gMonitorMode = false;
  SPECTRUM_Y = 8;

  analyserSettingsLoad();
  ANALYSERMENU_SetDbmMin(aSettings.dbMin);
  ANALYSERMENU_SetDbmMax(aSettings.dbMax);

  if (aSettings.delay > 0 && aSettings.delay <= DELAY_MAX_US) {
    delay = aSettings.delay;
  } else {
    delay = DELAY_DEFAULT_US;
  }

  waterfallOn = (aSettings.waterfallEnabled != 0);
  wfReset();

  analyserMode =
      (aSettings.mode < MODE_COUNT) ? (AnalyserMode)aSettings.mode : MODE_SCAN;
  peaksFound = 0;

  range.step = RADIO_GetParam(ctx, PARAM_STEP);

  if (isValidRange(aSettings.lastRangeStart, aSettings.lastRangeEnd)) {
    range.start = aSettings.lastRangeStart;
    range.end = aSettings.lastRangeEnd;
  } else {
    range.start = DEFAULT_RANGE_START;
    range.end = range.start + StepFrequencyTable[range.step] * LCD_WIDTH;
  }

  msm = &vfo->msm;
  msm->f = range.start;
  targetF = range.start;

  markerA = (Marker){0};
  markerB = (Marker){0};

  showDbmTuner = false;
  showSqTuner = false;

  scanAwaitingSettle = false;
  settingsDirty = false;
  analyserSaveTime = 0;
  lastSquelchLevel = SQ_LEVEL_INVALID;

  applySquelchPreset();

  SCAN_SetMode(SCAN_MODE_NONE);
  SPECTRUM_H = needSplit() ? SPECTRUM_H_SPLIT : SPECTRUM_H_FULL;
  SP_Init(&range);
  BANDS_RangePush(range);
}

void ANALYSER_deinit(void) {
  aSettings.dbMin = ANALYSERMENU_GetDbmMin();
  aSettings.dbMax = ANALYSERMENU_GetDbmMax();
  aSettings.delay = delay;
  aSettings.lastRangeStart = range.start;
  aSettings.lastRangeEnd = range.end;
  aSettings.waterfallEnabled = waterfallOn ? 1 : 0;
  aSettings.mode = (uint8_t)analyserMode;
  analyserSettingsSave();
}

// ── Measurement / scan ─────────────────────────────────────────────────────

static void measure(void) {
  msm->rssi = RADIO_GetRSSI(ctx);
  msm->noise = RADIO_GetNoise(ctx);
  msm->glitch = RADIO_GetGlitch(ctx);

  if (listen) {
    msm->open = true;
  } else {
    msm->open =
        (msm->rssi >= sq.ro) && (msm->noise < sq.no) && (msm->glitch < sq.go);
  }
  if (gMonitorMode) {
    msm->open = true;
  }
  LOOT_Update(msm);
  autoSqSample();
}

static void updateListening(void) {
  static uint32_t lastListenUpdate;
  if (Now() - lastListenUpdate >= SQL_DELAY) {
    measure();
    lastListenUpdate = Now();
  }
}

static void updateScan(void) {
  if (delay <= DELAY_BLOCKING_MAX_US) {
    // Быстрая ветка: блокирующая задержка до 10мс
    RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
    RADIO_SetParam(ctx, PARAM_FREQUENCY, msm->f, false);
    RADIO_ApplySettings(ctx);
    SYSTICK_DelayUs(delay);
  } else {
    // Медленная ветка: неблокирующее ожидание settle
    if (!scanAwaitingSettle) {
      RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
      RADIO_SetParam(ctx, PARAM_FREQUENCY, msm->f, false);
      RADIO_ApplySettings(ctx);
      scanWaitUntil = Now() + (delay / 1000);
      scanAwaitingSettle = true;
      return;
    }
    if (Now() < scanWaitUntil)
      return;
    scanAwaitingSettle = false;
  }

  measure();
  if (!still) {
    SP_AddPoint(msm);
    wfOnMeasure();
  }
  LOOT_Update(msm);

  if (still)
    return;

  msm->f += StepFrequencyTable[range.step];

  if (msm->f > range.end) {
    wfOnSweepEnd();
    autoSqOnSweep();
    if (analyserMode == MODE_PEAKS || analyserMode == MODE_SCAN_LISTEN) {
      updatePeaks();
      updateMarkersFromPeaks();
    }
    msm->f = range.start;
    gRedrawScreen = true;
    SP_Begin();
  }
}

void ANALYSER_update(void) {
  applySquelchPreset();
  ANALYSER_UpdateSave();

  // Deferred single-tap KEY_6 → lockToPeak, если второй тап не пришёл
  if (key6PendingSingle &&
      (Now() - key6PendingSingle) >= KEY6_DOUBLE_TAP_MS) {
    key6PendingSingle = 0;
    lockToPeak();
  }

  bool shouldListen = still || (analyserMode == MODE_SCAN_LISTEN);

  if (shouldListen) {
    if (vfo->is_open) {
      updateListening();
    } else {
      updateScan();
    }
    if (vfo->is_open != msm->open) {
      vfo->is_open = msm->open;
      gRedrawScreen = true;
      if (msm->open) {
        targetF = msm->f;
      }
      RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
    }
  } else {
    // В SCAN и PEAKS не слушаем — гарантируем выключение аудио
    if (vfo->is_open) {
      vfo->is_open = false;
      RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
    }
    updateScan();
  }
}

// ── Render ─────────────────────────────────────────────────────────────────

// Частота в единицах 10Гц → МГц с 3 знаками после точки (kHz-точность)
static void printFreqKHz(uint8_t x, uint8_t y, uint8_t pos, uint32_t f) {
  PrintSmallEx(x, y, pos, C_FILL, "%u.%03u", f / 100000u, (f / 100u) % 1000u);
}

static void renderBottomFreq(void) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  bool showCur = (Now() < cursorRangeTimeout);
  Band r = CUR_GetRange(&range, step);

  uint32_t leftF = showCur ? r.start : range.start;
  uint32_t rightF = showCur ? r.end : range.end;

  printFreqKHz(1, LCD_HEIGHT - 2, POS_L, leftF);
  printFreqKHz(LCD_WIDTH - 1, LCD_HEIGHT - 2, POS_R, rightF);

  char buf[16];
  // Центр: только ΔF когда оба маркера стоят
  if (markerA.f && markerB.f) {
    uint32_t dF = (markerB.f > markerA.f) ? (markerB.f - markerA.f)
                                          : (markerA.f - markerB.f);
    mhzToS(buf, dF);
    PrintSmallEx(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, C_FILL, "d=%s", buf);
  }
}

static void renderSqTuner(void) {
  const char *labels[] = {"R", "N", "G"};
  uint16_t values[] = {sq.ro, sq.no, sq.go};

  for (uint8_t i = 0; i < 3; i++) {
    char sel = (i == sqEditParam) ? '<' : ' ';
    PrintSmallEx(LCD_WIDTH - 1, 18 + 6 * i, POS_R, C_FILL, "%s %3u%c",
                 labels[i], values[i], sel);
  }
  PrintSmallEx(LCD_WIDTH - 1, 18 + 6 * 3, POS_R, C_FILL, "s=%u", sqStp);
  PrintSmallEx(LCD_WIDTH - 1, 18 + 6 * 4, POS_R, C_FILL, "L=%u", sqEditLevel);
}

// Оверлей dBm tuner: значения max/min с подсказкой клавиш в скобках.
// Рисуется по центру на месте UI_DrawLoot, поверх любых баров —
// поэтому сначала очищаем область и обводим рамкой для читаемости.
static void renderDbmTuner(void) {
  int16_t dbMin = ANALYSERMENU_GetDbmMin();
  int16_t dbMax = ANALYSERMENU_GetDbmMax();

  const uint8_t bw = 76;
  const uint8_t bh = 15;
  const uint8_t bx = LCD_XCENTER - bw / 2;
  const uint8_t by = 11;

  FillRect(bx, by, bw, bh, C_CLEAR);
  DrawRect(bx, by, bw, bh, C_FILL);

  PrintSmallEx(LCD_XCENTER, by + 5, POS_C, C_FILL, "max=%d (3/9)", dbMax);
  PrintSmallEx(LCD_XCENTER, by + 11, POS_C, C_FILL, "min=%d (2/8)", dbMin);
}

static void renderPeakMarker(VMinMax v) {
  uint32_t f = SP_GetPeakF();
  uint16_t rssi = SP_GetPeakRssi();
  if (!rssi)
    return;

  SP_RenderArrow(f);
  FSmall(0, 12 + 6, POS_L, f);
  PrintSmallEx(0, 12 + 6 + 6, POS_L, C_FILL, "%ddBm", Rssi2DBm(rssi));
}

// Таблица пиков под спектром (режим PEAKS).
// Y-позиции рассчитываются от SPECTRUM_H_SPLIT: 3 строки × 6 пикс в area 20
// пикс.
static void renderPeaksTable(void) {
  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H_SPLIT + WF_GAP;
  const char labels[PEAKS_N] = {'A', 'B', '3'};

  for (uint8_t i = 0; i < peaksFound && i < PEAKS_N; i++) {
    uint32_t f = SP_X2F(peaks[i].x);
    int16_t dbm = Rssi2DBm(peaks[i].rssi);
    uint16_t n = SP_GetPointNoise(peaks[i].x);
    uint16_t g = SP_GetPointGlitch(peaks[i].x);
    PrintSmallEx(0, y0 + 5 + i * 6, POS_L, C_FILL, "%c %u.%03u %3d %3u %3u %3u",
                 labels[i], f / 100000u, (f / 100u) % 1000u, dbm, peaks[i].rssi,
                 n, g);
  }
}
// СТАЛО:
static void renderMarkersTable(void) {
  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H_SPLIT + WF_GAP;
  const Marker *markers[2] = {&markerA, &markerB};
  const char labels[2] = {'A', 'B'};

  for (uint8_t i = 0; i < 2; i++) {
    const Marker *m = markers[i];
    if (!m->f)
      continue; // маркер не установлен

    uint8_t x = SP_F2X(m->f);
    uint16_t rssi = (x != 0xFF) ? SP_GetPointRSSI(x) : 0;
    uint16_t n = (x != 0xFF) ? SP_GetPointNoise(x) : 0;
    uint16_t g = (x != 0xFF) ? SP_GetPointGlitch(x) : 0;
    int16_t dbm = Rssi2DBm(rssi);

    PrintSmallEx(0, y0 + 5 + i * 6, POS_L, C_FILL,
                 "%c%c %u.%03u %3d %3u %3u %3u", m->manual ? '*' : ' ',
                 labels[i], m->f / 100000u, m->f / 100u % 1000u, dbm, rssi, n,
                 g);
  }
}

// Info-блок для SCAN_LISTEN: loot-инфа + RSSI bar в area под спектром
static void renderScanListenInfo(void) {
  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H_SPLIT + WF_GAP;
  if (gLastActiveLoot) {
    UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, y0 + 5, POS_C);
  }
  UI_RSSIBar(y0 + 12);
}

// RSSI bar в area под спектром (STILL/listen)
static void renderStillRssiBar(void) {
  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H_SPLIT + WF_GAP;
  UI_RSSIBar(y0 + 6);
}

static void renderStillInfo(void) {
  SP_RenderArrow(RADIO_GetParam(ctx, PARAM_FREQUENCY));
  PrintMediumEx(LCD_XCENTER, 14, POS_C, C_FILL,
                RADIO_GetParamValueString(ctx, PARAM_FREQUENCY));

  PrintSmallEx(LCD_XCENTER, 12 + 6 * 2, POS_C, C_FILL, "R%u N%u G%u", msm->rssi,
               msm->noise, msm->glitch);

  if (gMonitorMode)
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 3, POS_C, C_FILL, "MONITOR");
}

// Стрелка + буква метки возле неё. В manual инвертируется для различия.
static void renderUserMarker(const Marker *m, char label) {
  if (!m->f)
    return;
  uint8_t x = SP_F2X(m->f);
  if (x == 0xFF)
    return;
  SP_RenderArrow(m->f);
  uint8_t ly = SPECTRUM_Y + SPECTRUM_H - 1;
  uint8_t lx = (x + 4 < LCD_WIDTH) ? x + 2 : x - 4;
  // Manual маркер подчёркиваем префиксом '*', auto — без
  PrintSmallEx(lx, ly, POS_L, C_FILL, "%s%c", m->manual ? "*" : "", label);
}

void ANALYSER_render(void) {
  STATUSLINE_RenderRadioSettings();

  VMinMax v = {.vMin = DBm2Rssi(ANALYSERMENU_GetDbmMin()),
               .vMax = DBm2Rssi(ANALYSERMENU_GetDbmMax())};
  SP_Render(&range, v);
  wfRender(v);
  renderBottomFreq();

  // Левый верх спектра: индикатор режима + delay
  PrintSmallEx(0, 12, POS_L, C_FILL, "%c %uus", getModeChar(), delay);
  PrintSmallEx(LCD_WIDTH - 1, 12, POS_R, C_FILL, "%s",
               RADIO_GetParamValueString(ctx, PARAM_STEP));

  if (still || listen) {
    renderStillInfo();
    if (!waterfallOn)
      renderStillRssiBar();
  } else {
    // scan-режимы: центр — peak marker (слева сверху), area под спектром — по
    // режиму
    switch (analyserMode) {
    case MODE_PEAKS:
      if (!waterfallOn)
        renderMarkersTable();
      break;
    case MODE_SCAN_LISTEN:
      renderScanListenInfo(); // всегда (waterfall в этом режиме не рисуется)
      break;
    default:
      renderPeakMarker(v);
      break;
    }
    if (Now() < cursorRangeTimeout)
      CUR_Render();
    if (showDbmTuner) {
      renderDbmTuner();
    }
  }

  // Пользовательские маркеры видны в scan-режимах
  if (!still && !showSqTuner) {
    renderUserMarker(&markerA, 'A');
    renderUserMarker(&markerB, 'B');
  }

  if (showSqTuner) {
    renderSqTuner();
    uint16_t threshold = 0;
    switch (sqEditParam) {
    case SQ_EDIT_RSSI:
      threshold = sq.ro;
      break;
    case SQ_EDIT_NOISE:
      threshold = sq.no;
      break;
    case SQ_EDIT_GLITCH:
      threshold = sq.go;
      break;
    }
    VMinMax gv = SP_GetGraphMinMax();
    SP_RenderLine(threshold, gv);
  }

  REGSMENU_Draw();
  ANALYSERMENU_Draw();
}
