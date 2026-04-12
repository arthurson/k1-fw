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
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdbool.h>
#include <string.h>

// ── Analyser state ─────────────────────────────────────────────────────────

typedef struct {
  int16_t dbMin;
  int16_t dbMax;
} AnalyserSettings;

static AnalyserSettings aSettings = {.dbMin = -120, .dbMax = -20};
static uint32_t analyserSaveTime;

static void analyserSettingsLoad(void) {
  STORAGE_LOAD("analyser.set", 0, &aSettings);
}

static void analyserSettingsSave(void) {
  STORAGE_SAVE("analyser.set", 0, &aSettings);
}

void ANALYSER_UpdateSave(void) {
  if (ANALYSERMENU_IsDirty()) {
    if (analyserSaveTime == 0) {
      analyserSaveTime = Now() + 1000;
    } else if (Now() > analyserSaveTime) {
      // Sync values and save
      aSettings.dbMin = ANALYSERMENU_GetDbmMin();
      aSettings.dbMax = ANALYSERMENU_GetDbmMax();
      analyserSettingsSave();
      ANALYSERMENU_ClearDirty();
      analyserSaveTime = 0;
    }
  } else {
    analyserSaveTime = 0;
  }
}

// ── Analyser state ─────────────────────────────────────────────────────────

static Band range;
static uint32_t targetF;
static uint32_t delay = 2200;
static bool still;
static bool listen;

// Static cursor: стрелка на фиксированной частоте, сканирование продолжается
static uint32_t staticCursorFreq; // 0 = не показывать
static uint16_t staticCursorRssi, staticCursorNoise, staticCursorGlitch;

// sq настройка
static SQL sq;
static uint8_t sqStp = 10; // шаг настройки
static bool showSqTuner;   // режим настройки sq

// Squelch editor state
typedef enum {
  SQ_EDIT_RSSI,
  SQ_EDIT_NOISE,
  SQ_EDIT_GLITCH,
} SqEditParam;

static SqEditParam sqEditParam = SQ_EDIT_RSSI;
static uint8_t sqEditLevel; // Current squelch level being edited (0-10)

static uint8_t lastSquelchLevel = 0xFF; // Track to detect changes

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
  if (newDelay > 90000)
    return 90000;
  return (uint32_t)newDelay;
}

static void applySquelchPreset(void) {
  if (ctx->squelch.value != lastSquelchLevel) {
    lastSquelchLevel = ctx->squelch.value;
    sqEditLevel = ctx->squelch.value;
    SquelchPreset preset = GetSqlPreset(sqEditLevel, ctx->frequency);
    sq.ro = preset.ro;
    sq.no = preset.no;
    sq.go = preset.go;
    sq.rc = sq.ro > SQ_HYSTERESIS ? sq.ro - SQ_HYSTERESIS : 0;
    sq.nc = sq.no + SQ_HYSTERESIS;
    sq.gc = sq.go + SQ_HYSTERESIS;
  }
}

static Measurement *msm;
static uint32_t cursorRangeTimeout = 0;

// -------------------------------------------------------------------------

// -------------------------------------------------------------------------

static void setRange(uint32_t fs, uint32_t fe) {
  range.step = RADIO_GetParam(ctx, PARAM_STEP);
  range.start = fs;
  range.end = fe;
  msm->f = range.start;
  SP_Init(&range);
  BANDS_RangeClear();
  BANDS_RangePush(range);
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

// Callback для FINPUT: показываем стрелку на введённой частоте
static void onStaticCursorFreq(uint32_t fs, uint32_t fe) {
  staticCursorFreq = fs;
}

// -------------------------------------------------------------------------
// Обработчики клавиш

static bool sqTunerKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_1:
  case KEY_7: {
    // Switch to RSSI mode and/or edit RSSI threshold
    graphMeasurement = GRAPH_RSSI;
    sqEditParam = SQ_EDIT_RSSI;
    int32_t delta = (key == KEY_1) ? sqStp : -(int32_t)sqStp;
    sq.ro = AdjustU(sq.ro, 0, 255, delta);
    sq.rc = sq.ro > SQ_HYSTERESIS ? sq.ro - SQ_HYSTERESIS : 0;
    return true;
  }
  case KEY_2:
  case KEY_8: {
    // Switch to Noise mode and/or edit Noise threshold
    graphMeasurement = GRAPH_NOISE;
    sqEditParam = SQ_EDIT_NOISE;
    int32_t delta = (key == KEY_2) ? sqStp : -(int32_t)sqStp;
    sq.no = AdjustU(sq.no, 0, 128, delta);
    sq.nc = sq.no + SQ_HYSTERESIS;
    return true;
  }
  case KEY_3:
  case KEY_9: {
    // Switch to Glitch mode and/or edit Glitch threshold
    graphMeasurement = GRAPH_GLITCH;
    sqEditParam = SQ_EDIT_GLITCH;
    int32_t delta = (key == KEY_3) ? sqStp : -(int32_t)sqStp;
    sq.go = AdjustU(sq.go, 0, 255, delta);
    sq.gc = sq.go + SQ_HYSTERESIS;
    return true;
  }
  case KEY_4:
  case KEY_6:
    // Change delay: 4=faster (+), 6=slower (-)
    delay = adjustDelay(key == KEY_4 ? 1 : -1);
    return true;
  case KEY_0:
    sqStp = (sqStp >= 100) ? 1 : sqStp * 10;
    return true;
  case KEY_MENU:
    // Save current squelch preset
    {
      SquelchPreset preset = {.ro = sq.ro, .no = sq.no, .go = sq.go};
      SQ_SavePreset(ctx->frequency >= SETTINGS_GetFilterBound() ? "/uhf.sq"
                                                                : "/vhf.sq",
                    sqEditLevel, &preset);
    }
    return true;
  default:
    break;
  }
  return false;
}

static bool stillModeKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_UP:
  case KEY_DOWN:
    // Tune frequency by step
    {
      uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
      int32_t delta = (key == KEY_UP) ? (int32_t)step : -(int32_t)step;
      if (gSettings.invertButtons)
        delta = -delta;
      targetF += delta;
      msm->f = targetF;
      RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
      RADIO_ApplySettings(ctx);
    }
    return true;
  case KEY_4:
  case KEY_6:
    // Tune frequency by 4/6 (short and long press)
    {
      uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
      int32_t delta = (key == KEY_4) ? (int32_t)step : -(int32_t)step;
      targetF += delta;
      msm->f = targetF;
      RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
      RADIO_ApplySettings(ctx);
    }
    return true;
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
  switch (key) {
  case KEY_UP:
  case KEY_DOWN:
    CUR_Move((key == KEY_UP) ^ gSettings.invertButtons);
    cursorRangeTimeout = Now() + 2000;
    return true;

  case KEY_1:
  case KEY_7:
    delay = adjustDelay(key == KEY_1 ? 1 : -1);
    return true;

  case KEY_2: { // zoom in по выделению курсора
    Band zoomed = CUR_GetRange(BANDS_RangePeek(), step);
    zoomed.step = RADIO_GetParam(ctx, PARAM_STEP); // берём текущий шаг
    BANDS_RangePush(zoomed);
    range = *BANDS_RangePeek();
    msm->f = range.start;
    SP_Init(&range);
    CUR_Reset();
    return true;
  }

  case KEY_8: // zoom out
    BANDS_RangePop();
    range = *BANDS_RangePeek();
    RADIO_SetParam(ctx, PARAM_STEP, range.step,
                   true); // восстанавливаем шаг родителя
    msm->f = range.start;
    SP_Init(&range);
    CUR_Reset();
    return true;

  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, false);
    range.step = RADIO_GetParam(ctx, PARAM_STEP);
    BANDS_RangePeek()->step = range.step; // синхронизируем стек
    SP_Init(&range);
    return true;

  case KEY_4:
    if (state == KEY_LONG_PRESSED) {
      // Long press: still to last active loot
      if (gLastActiveLoot) {
        still = true;
        listen = true;
        targetF = msm->f = gLastActiveLoot->f;
        RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
        RADIO_ApplySettings(ctx);
      }
    } else if (state == KEY_RELEASED) {
      // Short press: toggle still
      still = !still;
      if (still) {
        listen = true;
        targetF = msm->f;
        RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
        RADIO_ApplySettings(ctx);
      }
    }
    return true;

  case KEY_6: // lock to peak
    lockToPeak();
    return true;

  case KEY_SIDE1:
    LOOT_BlacklistLast();
    return true;
  case KEY_SIDE2:
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

  // Analyser menu (long KEY_0)
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
        SquelchPreset preset = GetSqlPreset(sqEditLevel, ctx->frequency);
        sq.ro = preset.ro;
        sq.no = preset.no;
        sq.go = preset.go;
        sq.rc = sq.ro > SQ_HYSTERESIS ? sq.ro - SQ_HYSTERESIS : 0;
        sq.nc = sq.no + SQ_HYSTERESIS;
        sq.gc = sq.go + SQ_HYSTERESIS;
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

// -------------------------------------------------------------------------

void ANALYSER_init(void) {
  SPECTRUM_Y = 8;
  SPECTRUM_H = 44;

  // Load persisted settings
  analyserSettingsLoad();
  ANALYSERMENU_SetDbmMin(aSettings.dbMin);
  ANALYSERMENU_SetDbmMax(aSettings.dbMax);

  range.step = RADIO_GetParam(ctx, PARAM_STEP);
  range.start = 43307500;
  range.end = range.start + StepFrequencyTable[range.step] * LCD_WIDTH;

  msm = &vfo->msm;
  msm->f = range.start;

  targetF = range.start;

  staticCursorFreq = 0;
  staticCursorRssi = 0;
  staticCursorNoise = 0;
  staticCursorGlitch = 0;

  // Apply squelch preset for current level
  applySquelchPreset();

  SCAN_SetMode(SCAN_MODE_NONE);
  SP_Init(&range);
  BANDS_RangePush(range);
}

void ANALYSER_deinit(void) {
  // Sync back from analysermenu and save
  aSettings.dbMin = ANALYSERMENU_GetDbmMin();
  aSettings.dbMax = ANALYSERMENU_GetDbmMax();
  analyserSettingsSave();
}

// -------------------------------------------------------------------------

static void measure(void) {
  msm->rssi = RADIO_GetRSSI(ctx);
  msm->noise = RADIO_GetNoise(ctx);
  msm->glitch = RADIO_GetGlitch(ctx);

  if (listen) {
    msm->open = true;
  } else {
    // программный шумодав по R/N/G
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
    /* if (gMonitorMode) {
      SP_ShiftGraph(-1);
      SP_AddGraphPoint(msm);
    } */
    lastListenUpdate = Now();
  }
}

static void updateScan(void) {
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, msm->f, false);
  RADIO_ApplySettings(ctx);
  SYSTICK_DelayUs(delay);

  measure();
  if (!still)
    SP_AddPoint(msm);
  LOOT_Update(msm);

  if (still)
    return;

  msm->f += StepFrequencyTable[range.step];

  if (msm->f > range.end) {
    msm->f = range.start;
    gRedrawScreen = true;
    SP_Begin();
  }
}

void ANALYSER_update(void) {
  // Check if squelch level changed and reload preset
  applySquelchPreset();

  // Debounced save of analyser settings
  ANALYSER_UpdateSave();

  // Сохраняем R/N/G для стрелки если частота совпала
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

// -------------------------------------------------------------------------

static void renderBottomFreq(void) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  bool showCur = (Now() < cursorRangeTimeout);
  Band r = CUR_GetRange(&range, step);

  uint32_t leftF = showCur ? r.start : range.start;
  uint32_t centerF =
      showCur ? CUR_GetCenterF(step) : RADIO_GetParam(ctx, PARAM_FREQUENCY);
  uint32_t rightF = showCur ? r.end : range.end;

  FSmall(1, LCD_HEIGHT - 2, POS_L, leftF);
  FSmall(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, centerF);
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

  // SP_RenderMarker(SP_FindPeakX(), v);
  SP_RenderArrow(f);

  FSmall(0, 12 + 6, POS_L, f);
  PrintSmallEx(0, 12 + 6 + 6, POS_L, C_FILL, "%ddBm", Rssi2DBm(rssi));
}

static void renderStillInfo(void) {
  SP_RenderArrow(RADIO_GetParam(ctx, PARAM_FREQUENCY));
  PrintMediumEx(LCD_XCENTER, 14, POS_C, C_FILL,
                RADIO_GetParamValueString(ctx, PARAM_FREQUENCY));

  // R N G текущего измерения на маркерной частоте
  PrintSmallEx(LCD_XCENTER, 12 + 6 * 2, POS_C, C_FILL, "R%u N%u G%u", msm->rssi,
               msm->noise, msm->glitch);

  if (gMonitorMode)
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 3, POS_C, C_FILL, "MONITOR");
}

static void renderStaticCursorInfo(void) {
  // Стрелка на фиксированной частоте
  SP_RenderArrow(staticCursorFreq);
  FSmall(0, 12 + 6, POS_L, staticCursorFreq);
  PrintSmallEx(0, 12 + 6 + 6, POS_L, C_FILL, "R%u N%u G%u", staticCursorRssi,
               staticCursorNoise, staticCursorGlitch);
}

void ANALYSER_render(void) {
  STATUSLINE_RenderRadioSettings();

  VMinMax v = {.vMin = DBm2Rssi(ANALYSERMENU_GetDbmMin()),
               .vMax = DBm2Rssi(ANALYSERMENU_GetDbmMax())};
  SP_Render(&range, v);
  renderBottomFreq();

  // задержка и шаг слева/справа
  PrintSmallEx(0, 12, POS_L, C_FILL, "%uus", delay);
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
    // маркер пика всегда виден в режиме сканирования
    renderPeakMarker(v);
    CUR_Render();
    if (gLastActiveLoot)
      UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, 14, POS_C);
  }

  if (showSqTuner) {
    renderSqTuner();
    // Show squelch threshold line on spectrum
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
