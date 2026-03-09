#include "scaner.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/bands.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/finput.h"
#include "../ui/lootlist.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <stdint.h>

static VMinMax minMaxRssi;
static uint32_t cursorRangeTimeout = 0;
static bool pttWasLongPressed = false;

static void setRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
  BANDS_RangeClear();
  SCAN_setRange(fs, fe);
  BANDS_RangePush(gCurrentBand);
}

static void initBand(void) {
  if (gCurrentBand.detached) {
    LogC(LOG_C_BRIGHT_YELLOW, "[i] [SCAN] Init withOUT detached band");
    gCurrentBand = BANDS_ByFrequency(RADIO_GetParam(ctx, PARAM_FREQUENCY));
    gCurrentBand.detached = true;
  } else {
    LogC(LOG_C_BRIGHT_YELLOW, "[i] [SCAN] Init with detached band");
    if (!gCurrentBand.start && !gCurrentBand.end) {
      gCurrentBand = DEFAULT_BAND;
    }
  }
  if (gCurrentBand.start == DEFAULT_BAND.start &&
      gCurrentBand.end == DEFAULT_BAND.end) {
    uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
    gCurrentBand.start = ctx->frequency - 64 * step;
    gCurrentBand.end = gCurrentBand.start + 128 * step;
  }
}

void SCANER_init(void) {
  gMonitorMode = false;
  SPECTRUM_Y = 8;
  SPECTRUM_H = 44;

  initBand();

  gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
  BANDS_RangeClear();
  BANDS_RangePush(gCurrentBand);

  SCAN_SetDelay(1200);

  SCAN_SetMode(SCAN_MODE_FREQUENCY);
  SCAN_Init(false);
}

void SCANER_update(void) {}

static bool handleLongPress(KEY_Code_t key) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];
  Band _b;

  switch (key) {
  case KEY_6:
    if (!gLastActiveLoot)
      return false;

    _b = gCurrentBand;
    _b.start = gLastActiveLoot->f - step * 64;
    _b.end = _b.start + step * 128;
    BANDS_RangePush(_b);
    SCAN_setBand(*BANDS_RangePeek());
    CUR_Reset();
    return true;

    /* case KEY_0:
      gChListFilter = TYPE_FILTER_BAND;
      APPS_run(APP_CH_LIST);
      return true; */

  case KEY_PTT:
    if (gSettings.keylock) {
      pttWasLongPressed = true;
      LOOT_WhitelistLast();
      SCAN_Next();
      return true;
    }
    return false;

  default:
    return false;
  }
}

static bool handleRepeatableKeys(KEY_Code_t key) {
  switch (key) {
  case KEY_1:
  case KEY_7:
    SCAN_SetDelay(
        AdjustU(SCAN_GetDelay(), 0, 10000, key == KEY_1 ? 100 : -100));
    return true;

  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_STEP, key == KEY_3, false);
    gCurrentBand.step = RADIO_GetParam(ctx, PARAM_STEP);
    SCAN_setBand(gCurrentBand);
    return true;

  case KEY_UP:
  case KEY_DOWN:
    CUR_Move(key == KEY_UP);
    cursorRangeTimeout = Now() + 2000;
    return true;

  default:
    return false;
  }
}

static bool handleLongPressCont(KEY_Code_t key) {
  switch (key) {
  case KEY_2:
  case KEY_8:
    CUR_Size(key == KEY_2);
    cursorRangeTimeout = Now() + 2000;
    return true;

  default:
    return false;
  }
}

static bool handlePTTRelease(void) {
  // Переход в VFO если не заблокирован и есть активный сигнал
  if (gLastActiveLoot && !gSettings.keylock) {
    uint32_t targetF = gLastActiveLoot->f; // сохраняем ДО
    APPS_run(APP_VFO1);
    RADIO_SetParam(ctx, PARAM_FREQUENCY, targetF, true);
    RADIO_ApplySettings(ctx);
    RADIO_SaveCurrentVFO(gRadioState);
    return true;
  }

  // Блокировка: короткое нажатие = blacklist
  if (gSettings.keylock && !pttWasLongPressed) {
    pttWasLongPressed = false;
    SCAN_NextBlacklist();
    return true;
  }

  return false;
}

static bool handleRelease(KEY_Code_t key) {
  uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];

  switch (key) {

  case KEY_5:
    gFInputCallback = setRange;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, true);
    gFInputValue1 = 0;
    gFInputValue1 = 0;
    FINPUT_init();
    gFInputActive = true;
    return true;

  case KEY_SIDE1:
    SCAN_NextBlacklist();
    return true;

  case KEY_SIDE2:
    SCAN_NextWhitelist();
    return true;

  case KEY_STAR:
    LOOTLIST_init();
    gLootlistActive = true;
    return true;

  case KEY_2:
    BANDS_RangePush(CUR_GetRange(BANDS_RangePeek(), step));
    SCAN_setBand(*BANDS_RangePeek());
    CUR_Reset();
    return true;

  case KEY_8:
    BANDS_RangePop();
    SCAN_setBand(*BANDS_RangePeek());
    CUR_Reset();
    return true;

  case KEY_PTT:
    return handlePTTRelease();

  default:
    return false;
  }
}

bool SCANER_key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED && REGSMENU_Key(key, state)) {
    return true;
  }

  if (state == KEY_PRESSED && key == KEY_PTT) {
    pttWasLongPressed = false;
  }

  if (state == KEY_LONG_PRESSED) {
    return handleLongPress(key);
  }

  if (state == KEY_LONG_PRESSED_CONT) {
    return handleLongPressCont(key);
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (handleRepeatableKeys(key)) {
      return true;
    }
  }

  if (state == KEY_RELEASED) {
    return handleRelease(key);
  }

  return false;
}

static void renderTopInfo(void) {
  const uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];

  if (gLastActiveLoot) {
    UI_DrawLoot(gLastActiveLoot, LCD_XCENTER, 14, POS_C);
  }

  PrintSmallEx(0, 12, POS_L, C_FILL, "%uus", SCAN_GetDelay());
  PrintSmallEx(LCD_WIDTH, 12, POS_R, C_FILL, "%u.%02uk", step / 100,
               step % 100);

  if (BANDS_RangeIndex() > 0) {
    PrintSmallEx(0, 18, POS_L, C_FILL, "Zoom %u", BANDS_RangeIndex() + 1);
  }

  PrintSmallEx(0, 24, POS_L, C_FILL, "CPS %u", SCAN_GetCps());
}

static void renderBottomFreq(uint32_t step) {
  Band r = CUR_GetRange(&gCurrentBand, step);
  bool showCurRange = (Now() < cursorRangeTimeout);

  uint32_t leftF = showCurRange ? r.start : gCurrentBand.start;
  uint32_t centerF = showCurRange ? CUR_GetCenterF(step)
                                  : RADIO_GetParam(ctx, PARAM_FREQUENCY);
  uint32_t rightF = showCurRange ? r.end : gCurrentBand.end;

  FSmall(1, LCD_HEIGHT - 2, POS_L, leftF);
  FSmall(LCD_XCENTER, LCD_HEIGHT - 2, POS_C, centerF);
  FSmall(LCD_WIDTH - 1, LCD_HEIGHT - 2, POS_R, rightF);
}

void SCANER_render(void) {
  const uint32_t step = StepFrequencyTable[RADIO_GetParam(ctx, PARAM_STEP)];

  STATUSLINE_RenderRadioSettings();

  // Установка диапазона для спектра
  minMaxRssi = SP_GetMinMax();

  SP_Render(&gCurrentBand, minMaxRssi);

  renderTopInfo();

  SP_RenderArrow(RADIO_GetParam(ctx, PARAM_FREQUENCY));

  renderBottomFreq(step);
  CUR_Render();

  if (vfo->is_open) {
    UI_RSSIBar(17);
  }

  REGSMENU_Draw();
}

void SCANER_deinit(void) {}
