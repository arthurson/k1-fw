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

static Band range;

static uint32_t targetF;
static uint32_t delay = 1200;

static bool still;
static bool listen;

// sq настройка
static SQL sq;
static uint8_t sqStp = 10; // шаг настройки
static bool showSqTuner;   // режим настройки sq

static Measurement *msm;
static uint32_t cursorRangeTimeout = 0;

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

// -------------------------------------------------------------------------
// Обработчики клавиш

static bool sqTunerKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_1:
  case KEY_7:
    sq.ro = AdjustU(sq.ro, 0, 255, key == KEY_1 ? sqStp : -(int32_t)sqStp);
    sq.rc = sq.ro > 4 ? sq.ro - 4 : 0;
    return true;
  case KEY_2:
  case KEY_8:
    // no: меньше → жёстче, KEY_2 ужесточает (уменьшает)
    sq.no = AdjustU(sq.no, 0, 255, key == KEY_2 ? -(int32_t)sqStp : sqStp);
    sq.nc = sq.no + 4;
    return true;
  case KEY_3:
  case KEY_9:
    sq.go = AdjustU(sq.go, 0, 255, key == KEY_3 ? -(int32_t)sqStp : sqStp);
    sq.gc = sq.go + 4;
    return true;
  case KEY_0:
    sqStp = (sqStp >= 100) ? 1 : sqStp * 10;
    return true;
  default:
    break;
  }
  return false;
}

static bool stillModeKey(KEY_Code_t key, Key_State_t state) {
  switch (key) {
  case KEY_SIDE1:
    gMonitorMode = !gMonitorMode;
    return true;
  case KEY_UP:
  case KEY_DOWN: {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    int32_t dir = ((key == KEY_UP) ^ gSettings.invertButtons) ? 1 : -1;
    targetF = msm->f = AdjustU(RADIO_GetParam(ctx, PARAM_FREQUENCY),
                               range.start, range.end, step * dir);
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
    RADIO_ApplySettings(ctx);
    return true;
  }
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
    delay = AdjustU(delay, 0, 10000, key == KEY_1 ? 100 : -100);
    return true;

  case KEY_2: // zoom in по выделению курсора
    BANDS_RangePush(CUR_GetRange(BANDS_RangePeek(), step));
    range = *BANDS_RangePeek();
    CUR_Reset();
    return true;

  case KEY_8: // zoom out
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

  case KEY_4: // still на позиции курсора
    still = !still;
    if (still) {
      targetF = CUR_GetCenterF(step);
      msm->f = targetF;
      RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
      RADIO_ApplySettings(ctx);
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
  }
  return false;
}

bool NEWSCAN_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state))
    return true;

  if (state == KEY_RELEASED) {
    if (key == KEY_EXIT) {
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
      return true;
    }
    if (key == KEY_5) {
      FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
      FINPUT_Show(setRange);
      return true;
    }
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

void NEWSCAN_init(void) {
  SPECTRUM_Y = 8;
  SPECTRUM_H = 44;

  range.step = RADIO_GetParam(ctx, PARAM_STEP);
  range.start = 43307500;
  range.end = range.start + StepFrequencyTable[range.step] * LCD_WIDTH;

  msm = &vfo->msm;
  msm->f = range.start;

  targetF = range.start;
  sq = GetSql(5); // начальный уровень шумодава

  SCAN_SetMode(SCAN_MODE_NONE);
  SP_Init(&range);
  BANDS_RangePush(range);
}

void NEWSCAN_deinit(void) {}

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

void NEWSCAN_update(void) {
  if (vfo->is_open) {
    updateListening();
  } else {
    updateScan();
  }

  // триггер: шумодав открылся во время скана → залипаем на частоте
  if (!still && !listen && msm->open) {
    still = true;
    listen = true;
    targetF = msm->f;
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, false);
    RADIO_ApplySettings(ctx);
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
  // правая колонка под шагом
  PrintSmallEx(LCD_WIDTH - 1, 18 + 6 * 0, POS_R, C_FILL, "R>=%u", sq.ro);
  PrintSmallEx(LCD_WIDTH - 1, 18 + 6 * 1, POS_R, C_FILL, "N< %u", sq.no);
  PrintSmallEx(LCD_WIDTH - 1, 18 + 6 * 2, POS_R, C_FILL, "G< %u", sq.go);
  PrintSmallEx(LCD_WIDTH - 1, 18 + 6 * 3, POS_R, C_FILL, "s=%u", sqStp);
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

  if (listen)
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 3, POS_C, C_FILL, "LISTEN");
  else
    PrintSmallEx(LCD_XCENTER, 12 + 6 * 3, POS_C, C_FILL, "STILL");
}

void NEWSCAN_render(void) {
  STATUSLINE_RenderRadioSettings();

  VMinMax v = SP_GetMinMax();
  SP_Render(&range, v);
  renderBottomFreq();

  // задержка и шаг слева/справа
  PrintSmallEx(0, 12, POS_L, C_FILL, "%uus", delay);
  PrintSmallEx(LCD_WIDTH - 1, 12, POS_R, C_FILL, "%s",
               RADIO_GetParamValueString(ctx, PARAM_STEP));

  if (still || listen) {
    if (gMonitorMode) {
      // скроллинг-граф как в vfo1
      const uint8_t gBase = 22;
      /* SPECTRUM_Y = gBase;
      SPECTRUM_H = LCD_HEIGHT - gBase - 8; */
      // SP_RenderGraph(DBm2Rssi(-120), DBm2Rssi(-50));
      UI_RSSIBar(12 + 7);
      /* SPECTRUM_Y = 8;
      SPECTRUM_H = 44; */
    }
    renderStillInfo();
  } else {
    // маркер пика всегда виден в режиме сканирования
    renderPeakMarker(v);
    CUR_Render();
    if (gLastActiveLoot)
      UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, 14, POS_C);
  }

  if (showSqTuner)
    renderSqTuner();

  REGSMENU_Draw();
}
