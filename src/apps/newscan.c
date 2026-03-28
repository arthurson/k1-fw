#include "newscan.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdbool.h>

// отладка шумодава (на одной частоте?)
// анализ эфира
// прослушивание в любом режиме через still

typedef enum {
  AM_ANALYZER,
  AM_SCAN,
  AM_SQ,

  AM_COUNT,
} AnalyzerMode;

static AnalyzerMode mode;
static const char *AM_NAMES[] = {
    [AM_ANALYZER] = "ANALYZER",
    [AM_SCAN] = "SCAN",
    [AM_SQ] = "SQ",
};

static Band range;
static VMinMax minMax;

static uint32_t targetF = 434 * MHZ;
static uint32_t delay = 1200;
static uint8_t stp = 10;
static SQL sq;
static bool still;
static bool listen;

static Measurement *msm;
static Measurement tgt[3];

static uint32_t cursorRangeTimeout = 0;

static void setTargetF(uint32_t fs, uint32_t _) { targetF = fs; }

static void setRange(uint32_t fs, uint32_t fe) {
  range.step = RADIO_GetParam(ctx, PARAM_STEP);
  range.start = fs;
  range.end = fe;
  msm->f = range.start;
  SP_Init(&range);
  BANDS_RangeClear();
  BANDS_RangePush(range);
}

bool sqModeKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_6:
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(setTargetF);
    return true;

  case KEY_SIDE1:
    LOOT_BlacklistLast();
    return true;

  case KEY_SIDE2:
    LOOT_WhitelistLast();
    return true;

  case KEY_4:
    still = !still;
    return true;

  case KEY_1:
  case KEY_7:
    sq.ro = AdjustU(sq.ro, 0, 255, key == KEY_1 ? stp : -stp);
    return true;
  case KEY_2:
  case KEY_8:
    sq.no = AdjustU(sq.no, 0, 255, key == KEY_2 ? stp : -stp);
    return true;
  case KEY_3:
  case KEY_9:
    sq.go = AdjustU(sq.go, 0, 255, key == KEY_3 ? stp : -stp);
    return true;
  case KEY_0:
    if (stp == 100) {
      stp = 1;
    } else {
      stp *= 10;
    }
    return true;
  default:
    break;
  }
  return false;
}

bool scanModeKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {

  case KEY_SIDE1:
    LOOT_BlacklistLast();
    return true;

  case KEY_SIDE2:
    LOOT_WhitelistLast();
    return true;

  case KEY_STAR:
    APPS_run(APP_LOOTLIST);
    return true;

  case KEY_1:
  case KEY_7:
    delay = AdjustU(delay, 0, 10000,
                    ((key == KEY_1) ^ gSettings.invertButtons) ? 100 : -100);
    return true;
  default:
    break;
  }
  return false;
}

bool analyzerModeKey(KEY_Code_t key, Key_State_t state) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  switch (key) {
  case KEY_UP:
  case KEY_DOWN:
    CUR_Move((key == KEY_UP) ^ gSettings.invertButtons);
    cursorRangeTimeout = Now() + 2000;
    return true;
  case KEY_1:
  case KEY_7:
    delay = AdjustU(delay, 0, 10000, key == KEY_1 ? 100 : -100);
    return true;
  case KEY_2:
    BANDS_RangePush(CUR_GetRange(BANDS_RangePeek(), step));
    range = *BANDS_RangePeek();
    CUR_Reset();
    return true;

  case KEY_4:
    still = !still;
    if (still) {
      targetF =
          CUR_GetCenterF(StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)]);
      msm->f = targetF;
    }
    return true;
  case KEY_6:
    listen = !listen;
    if (listen) {
      still = true;
    }
    if (still) {
      targetF =
          CUR_GetCenterF(StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)]);
      msm->f = targetF;
    }
    return true;
  case KEY_8:
    BANDS_RangePop();
    range = *BANDS_RangePeek();
    CUR_Reset();
    return true;
  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, false);
    range.step = RADIO_GetParam(ctx, PARAM_STEP);
    SP_Init(&range);
    return true;
  }
  return false;
}

bool stillModeKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {

  case KEY_UP:
  case KEY_DOWN:
    targetF = msm->f =
        AdjustU(RADIO_GetParam(ctx, PARAM_FREQUENCY), range.start, range.end,
                StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)] *
                    (((key == KEY_UP) ^ gSettings.invertButtons) ? 1 : -1));
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
    RADIO_ApplySettings(ctx);
    return true;

  case KEY_1:
  case KEY_7:
    delay = AdjustU(delay, 0, 10000,
                    ((key == KEY_1) ^ gSettings.invertButtons) ? 100 : -100);
    return true;
  default:
    break;
  }
  return false;
}

bool NEWSCAN_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state)) {
    return true;
  }
  if (state == KEY_RELEASED) {
    if (key == KEY_EXIT && (listen || still)) {
      listen = false;
      still = false;
      return true;
    }
    if (key == KEY_F) {
      mode = IncDecU(mode, 0, AM_COUNT, true);
      return true;
    }
    if (key == KEY_5) {
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
      FINPUT_Show(setRange);
      return true;
    }
  }
  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (still && stillModeKey(key, state)) {
      return true;
    }
    switch (mode) {
    case AM_SCAN:
      return scanModeKey(key, state);
    case AM_SQ:
      return sqModeKey(key, state);
    case AM_ANALYZER:
      return analyzerModeKey(key, state);
    default:
      break;
    }
  }
  return false;
}

void NEWSCAN_init(void) {
  SPECTRUM_Y = 8;
  SPECTRUM_H = 44;

  range.step = RADIO_GetParam(ctx, PARAM_STEP);
  range.start = 43307500;
  range.end = range.start + StepFrequencyTable[range.step] * LCD_WIDTH;
  msm = &vfo->msm;
  msm->f = range.start;

  sq = GetSql(9);

  SCAN_SetMode(SCAN_MODE_NONE);
  SP_Init(&range);
  BANDS_RangePush(range);
}

void NEWSCAN_deinit(void) {}

void measure() {
  msm->rssi = RADIO_GetRSSI(ctx);
  msm->noise = RADIO_GetNoise(ctx);
  msm->glitch = RADIO_GetGlitch(ctx);

  if (listen) {
    msm->open = true;
  } else if (mode == AM_ANALYZER) {
    msm->open = false;
  } else {
    msm->open = msm->rssi >= sq.ro && msm->noise < sq.no && msm->glitch < sq.go;
  }
  LOOT_Update(msm);

  if (mode == AM_SQ) {
    if (msm->f == targetF - StepFrequencyTable[range.step]) {
      tgt[0] = *msm;
    }
    if (msm->f == targetF) {
      tgt[1] = *msm;
    }
    if (msm->f == targetF + StepFrequencyTable[range.step]) {
      tgt[2] = *msm;
    }
  }
}

void updateListening() {
  static uint32_t lastListenUpdate;
  if (Now() - lastListenUpdate >= SQL_DELAY) {
    measure();
    lastListenUpdate = Now();
  }
}

void updateScan() {
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, msm->f, false);
  RADIO_ApplySettings(ctx);
  SYSTICK_DelayUs(delay);

  measure();
  SP_AddPoint(msm);
  LOOT_Update(msm);

  if (still) {
    return;
  }

  msm->f += StepFrequencyTable[range.step];

  if (msm->f > range.end) {
    msm->f = range.start;
    gRedrawScreen = true;
    SP_Begin();
  }
}

void NEWSCAN_update(void) {
  if (vfo->is_open) {
    updateListening();
  } else {
    updateScan();
  }
  if (vfo->is_open != msm->open) {
    // Log("OPEN=%u", msm->open);
    vfo->is_open = msm->open;
    gRedrawScreen = true;
    targetF = msm->f;
    RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  }
}

static void renderBottomFreq() {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  Band r = CUR_GetRange(&range, step);
  bool showCurRange = (Now() < cursorRangeTimeout);

  uint32_t leftF = showCurRange ? r.start : range.start;
  uint32_t centerF = showCurRange ? CUR_GetCenterF(step)
                                  : RADIO_GetParam(ctx, PARAM_FREQUENCY);
  uint32_t rightF = showCurRange ? r.end : range.end;

  FSmall(1, LCD_HEIGHT - 2, POS_L, leftF);
  FSmall(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, centerF);
  FSmall(LCD_WIDTH - 1, LCD_HEIGHT - 2, POS_R, rightF);
}

void renderSqMode() {
  PrintSmall(0, 12 + 6 * 0, "R %u", sq.ro);
  PrintSmall(0, 12 + 6 * 1, "N %u", sq.no);
  PrintSmall(0, 12 + 6 * 2, "G %u", sq.go);
  PrintSmall(0, 12 + 6 * 3, "STP %u", stp);
}

void renderRNGMode() {
  PrintSmallEx(LCD_XCENTER, 12 + 6 * 0, POS_C, C_FILL, "%3u %3u %3u",
               tgt[0].rssi, tgt[1].rssi, tgt[2].rssi);
  PrintSmallEx(LCD_XCENTER, 12 + 6 * 1, POS_C, C_FILL, "%3u %3u %3u",
               tgt[0].noise, tgt[1].noise, tgt[2].noise);
  PrintSmallEx(LCD_XCENTER, 12 + 6 * 2, POS_C, C_FILL, "%3u %3u %3u",
               tgt[0].glitch, tgt[1].glitch, tgt[2].glitch);
}

void renderMinMax() {
  PrintSmallEx(0, 12 + 6 * 1, POS_L, C_FILL, "%u", SP_GetRssiMax());
  PrintSmallEx(0, 12 + 6 * 2, POS_L, C_FILL, "%u", SP_GetNoiseFloor());
}

void renderSpectrum() {
  SP_Render(&range, SP_GetMinMax());
  renderBottomFreq();
}

void renderScanMode() { PrintSmallEx(0, 12, POS_L, C_FILL, "%uus", delay); }

void renderStillMode() {
  PrintSmallEx(LCD_XCENTER, 12 + 6 * 3, POS_C, C_FILL, "%s", "STILL");
}

void NEWSCAN_render(void) {
  STATUSLINE_RenderRadioSettings();

  renderSpectrum();

  PrintSmallEx(LCD_WIDTH - 1, 12 + 6 * 2, POS_R, C_FILL, "%s", AM_NAMES[mode]);
  if (listen) {
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 2, POS_C, C_FILL, "LISTEN MODE");
  }

  if (still || listen) {
    renderStillMode();
    SP_RenderArrow(RADIO_GetParam(ctx, PARAM_FREQUENCY));
    PrintMediumEx(LCD_XCENTER, 14, POS_C, C_FILL,
                  RADIO_GetParamValueString(ctx, PARAM_FREQUENCY));
  }

  switch (mode) {
  case AM_ANALYZER:
    if (!still) {
      CUR_Render();
    }
    renderMinMax();
    renderScanMode();
    PrintSmallEx(LCD_WIDTH - 1, 12 + 6 * 0, POS_R, C_FILL, "%s",
                 RADIO_GetParamValueString(ctx, PARAM_STEP));
    break;
  case AM_SCAN:
    renderScanMode();
    if (gLastActiveLoot) {
      UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, 14, POS_C);
    }
    break;
  case AM_SQ:
    renderSqMode();
    renderRNGMode();
    SP_RenderArrow(targetF);
    break;
  default:
    break;
  }

  REGSMENU_Draw();
}
