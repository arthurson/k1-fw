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
#include "apps.h"
#include <stdbool.h>
#include <string.h>

// ── Constants ──────────────────────────────────────────────────────────────

#define ANALYSER_SETTINGS_MAGIC 0xA51Bu // bump при смене структуры
#define SQ_LEVEL_INVALID 0xFF
#define CURSOR_INFO_TIMEOUT_MS 2000
#define SETTINGS_SAVE_DEBOUNCE_MS 1000
#define DELAY_BLOCKING_MAX_US 10000 // ≤10мс — блокирующая задержка
#define DELAY_DEFAULT_US 2200
#define DELAY_MAX_US 90000
#define DEFAULT_RANGE_START 43307500u

// Waterfall geometry
#define WF_H 16
#define WF_GAP 1
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
  uint8_t _pad[3];
} AnalyserSettings;

static AnalyserSettings aSettings = {
    .magic = ANALYSER_SETTINGS_MAGIC,
    .dbMin = -120,
    .dbMax = -20,
    .delay = DELAY_DEFAULT_US,
    .lastRangeStart = 0,
    .lastRangeEnd = 0,
    .waterfallEnabled = 0,
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

// Static cursor: стрелка на фиксированной частоте, сканирование продолжается
static uint32_t staticCursorFreq;
static uint16_t staticCursorRssi, staticCursorNoise, staticCursorGlitch;

// Dual markers A/B
static uint32_t markerAF;
static uint32_t markerBF;

// Waterfall
static bool waterfallOn;
static uint8_t wfBuf[WF_H][LCD_WIDTH]; // [row][x] = rssi clamped to 0..255
static uint8_t wfHead; // текущая (пишущаяся) строка
static uint8_t
    wfFilled; // кол-во полностью заполненных старых строк (0..WF_H-1)

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

// ── Markers ────────────────────────────────────────────────────────────────

static inline bool freqInRange(uint32_t f) {
  return f >= range.start && f <= range.end;
}

// Цикл: -- → A → A+B → --
static void toggleMarker(uint32_t f) {
  if (!markerAF) {
    markerAF = f;
    return;
  }
  if (!markerBF) {
    markerBF = f;
    return;
  }
  markerAF = 0;
  markerBF = 0;
}

// ── Waterfall ──────────────────────────────────────────────────────────────

static void wfReset(void) {
  memset(wfBuf, 0, sizeof(wfBuf));
  wfHead = 0;
  wfFilled = 0;
}

// Пишем точку текущего измерения в текущую строку waterfall
static void wfOnMeasure(void) {
  if (!waterfallOn)
    return;
  uint8_t x = SP_F2X(msm->f);
  if (x == 0xFF)
    return;
  uint16_t r = msm->rssi;
  wfBuf[wfHead][x] = (r > 255) ? 255 : (uint8_t)r;
}

// Свип завершён — сдвигаем голову, обнуляем новую строку
static void wfOnSweepEnd(void) {
  if (!waterfallOn)
    return;
  if (wfFilled < WF_H - 1)
    wfFilled++;
  wfHead = (wfHead + 1) % WF_H;
  memset(wfBuf[wfHead], 0, LCD_WIDTH);
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

// Рендер: row=0 — текущая (частично заполненная), вниз — старшие
static void wfRender(VMinMax v) {
  if (!waterfallOn)
    return;
  uint8_t y0 = SPECTRUM_Y + SPECTRUM_H + WF_GAP;
  uint16_t thr16 = (v.vMin + v.vMax) / 2;
  uint8_t thr = (thr16 > 255) ? 255 : (uint8_t)thr16;

  uint8_t rows = wfFilled + 1;
  if (rows > WF_H)
    rows = WF_H;

  for (uint8_t r = 0; r < rows; r++) {
    uint8_t rowIdx = (wfHead + WF_H - r) % WF_H;
    uint8_t y = y0 + r;
    const uint8_t *line = wfBuf[rowIdx];
    for (uint8_t x = 0; x < LCD_WIDTH; x++) {
      if (line[x] > thr) {
        // TODO: замените PutPixel на функцию вашего st7565-драйвера
        // если имя отличается (например DrawPixel / PxSet / и т.п.)
        PutPixel(x, y, 1);
      }
    }
  }
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
  analyserSettingsSave();
  settingsDirty = false;
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

static void onStaticCursorFreq(uint32_t fs, uint32_t fe) {
  staticCursorFreq = fs;
}

// ── Mode indicator ─────────────────────────────────────────────────────────

static char getModeChar(void) {
  if (showSqTuner)
    return 'Q';
  if (staticCursorFreq)
    return 'C';
  if (listen)
    return 'L';
  if (still)
    return 'T';
  return 'S';
}

// ── Key handlers ───────────────────────────────────────────────────────────

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
      }
    } else if (state == KEY_RELEASED) {
      still = !still;
      if (still) {
        listen = true;
        targetF = msm->f;
        RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
        RADIO_ApplySettings(ctx);
      }
    }
    return true;

  case KEY_6:
    lockToPeak();
    return true;

  case KEY_SIDE1:
    if (state == KEY_LONG_PRESSED) {
      wfSetEnabled(!waterfallOn);
      return true;
    }
    LOOT_BlacklistLast();
    return true;
  case KEY_SIDE2:
    if (state == KEY_LONG_PRESSED) {
      uint32_t f = CUR_GetCenterF(step);
      if (f)
        toggleMarker(f);
      return true;
    }
    LOOT_WhitelistLast();
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
      if (staticCursorFreq) {
        staticCursorFreq = 0;
        return true;
      }
      if (listen) {
        listen = false;
        return true;
      }
      if (still) {
        still = false;
        return true;
      }
      if (showSqTuner) {
        showSqTuner = false;
        return true;
      }
    }
    if (key == KEY_F) {
      showSqTuner = !showSqTuner;
      if (showSqTuner) {
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

  // Static cursor: долгий KEY_5 в режиме сканирования → ввод частоты
  if (!still && key == KEY_5 && state == KEY_LONG_PRESSED) {
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(onStaticCursorFreq);
    return true;
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED ||
      state == KEY_LONG_PRESSED_CONT) {
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
  ANALYSERMENU_SetDirtyCallback(markSettingsDirty);

  if (aSettings.delay > 0 && aSettings.delay <= DELAY_MAX_US) {
    delay = aSettings.delay;
  } else {
    delay = DELAY_DEFAULT_US;
  }

  waterfallOn = (aSettings.waterfallEnabled != 0);
  SPECTRUM_H = waterfallOn ? SPECTRUM_H_SPLIT : SPECTRUM_H_FULL;
  wfReset();

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

  staticCursorFreq = 0;
  staticCursorRssi = 0;
  staticCursorNoise = 0;
  staticCursorGlitch = 0;

  markerAF = 0;
  markerBF = 0;

  scanAwaitingSettle = false;
  settingsDirty = false;
  analyserSaveTime = 0;
  lastSquelchLevel = SQ_LEVEL_INVALID;

  applySquelchPreset();

  SCAN_SetMode(SCAN_MODE_NONE);
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
    msm->f = range.start;
    gRedrawScreen = true;
    SP_Begin();
  }
}

void ANALYSER_update(void) {
  applySquelchPreset();
  ANALYSER_UpdateSave();

  if (staticCursorFreq && msm->f == staticCursorFreq) {
    staticCursorRssi = msm->rssi;
    staticCursorNoise = msm->noise;
    staticCursorGlitch = msm->glitch;
  }

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
}

// ── Render ─────────────────────────────────────────────────────────────────

static void renderBottomFreq(void) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  bool showCur = (Now() < cursorRangeTimeout);
  Band r = CUR_GetRange(&range, step);

  uint32_t leftF = showCur ? r.start : range.start;
  uint32_t centerF =
      showCur ? CUR_GetCenterF(step) : RADIO_GetParam(ctx, PARAM_FREQUENCY);
  uint32_t rightF = showCur ? r.end : range.end;

  FSmall(1, LCD_HEIGHT - 2, POS_L, leftF);

  // Если оба маркера выставлены — центр занимает ΔF
  if (markerAF && markerBF) {
    uint32_t dF =
        (markerBF > markerAF) ? (markerBF - markerAF) : (markerAF - markerBF);
    PrintSmallEx(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, C_FILL, "d=%u", dF);
  } else {
    FSmall(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, centerF);
  }

  FSmall(LCD_WIDTH - 1, LCD_HEIGHT - 2, POS_R, rightF);
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

static void renderPeakMarker(VMinMax v) {
  uint32_t f = SP_GetPeakF();
  uint16_t rssi = SP_GetPeakRssi();
  if (!rssi)
    return;

  SP_RenderArrow(f);
  FSmall(0, 12 + 6, POS_L, f);
  PrintSmallEx(0, 12 + 6 + 6, POS_L, C_FILL, "%ddBm", Rssi2DBm(rssi));
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

static void renderStaticCursorInfo(void) {
  SP_RenderArrow(staticCursorFreq);
  FSmall(0, 12 + 6, POS_L, staticCursorFreq);
  PrintSmallEx(0, 12 + 6 + 6, POS_L, C_FILL, "R%u N%u G%u", staticCursorRssi,
               staticCursorNoise, staticCursorGlitch);
}

// Стрелка + буква метки возле неё
static void renderUserMarker(uint32_t f, char label) {
  if (!f)
    return;
  uint8_t x = SP_F2X(f);
  if (x == 0xFF)
    return;
  SP_RenderArrow(f);
  // Подпись у нижней кромки спектра, сдвинута на пиксель от стрелки;
  // у правого края рисуем слева от стрелки чтобы не вылезать за LCD_WIDTH
  uint8_t ly = SPECTRUM_Y + SPECTRUM_H - 1;
  uint8_t lx = (x + 4 < LCD_WIDTH) ? x + 2 : x - 4;
  PrintSmallEx(lx, ly, POS_L, C_FILL, "%c", label);
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

  if (staticCursorFreq) {
    renderStaticCursorInfo();
  } else if (still || listen) {
    if (gMonitorMode) {
      UI_RSSIBar(24);
    }
    renderStillInfo();
  } else {
    renderPeakMarker(v);
    CUR_Render();
    if (gLastActiveLoot)
      UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, 14, POS_C);
  }

  // Пользовательские маркеры видны только в analyzer-режиме
  if (!still && !showSqTuner && !staticCursorFreq) {
    renderUserMarker(markerAF, 'A');
    renderUserMarker(markerBF, 'B');
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
