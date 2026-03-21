#include "fc.h"
#include "../dcs.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/lootlist.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "apps.h"
#include <stdint.h>

#define FM_BAND_LOW (88 * MHZ)
#define FM_BAND_HIGH (108 * MHZ)
#define FC_HZ_LOW 0x244
#define FC_HZ_HIGH 0x580

static const uint8_t REQUIRED_FREQUENCY_HITS = 2;
static const uint8_t FILTER_SWITCH_INTERVAL = REQUIRED_FREQUENCY_HITS;

static const char *FILTER_NAMES[] = {
    [FILTER_OFF] = "ALL",
    [FILTER_VHF] = "VHF",
    [FILTER_UHF] = "UHF",
};

static Filter filter = FILTER_VHF;
static bool bandAutoSwitch = true;
static uint32_t bound;
static bool isScanning = true;
static uint32_t currentFrequency = 0;
static uint32_t lastDetectedFrequency = 0;
static uint8_t frequencyHits = 0;
static uint8_t filterSwitchCounter = 0;
static uint16_t hz = FC_HZ_LOW;
static uint32_t fcTimeuot;
static uint32_t fcListenTimer;

static void enableScan(void) {
  BK4819_EnableFrequencyScanEx2(gSettings.fcTime, hz);
  isScanning = true;
}

static void disableScan(void) {
  BK4819_DisableFrequencyScan();
  BK4819_RX_TurnOn();
  isScanning = false;
}

static void switchFilter(void) {
  filter = (filter == FILTER_VHF) ? FILTER_UHF : FILTER_VHF;
  BK4819_SelectFilterEx(filter);
  filterSwitchCounter = 0;
  gRedrawScreen = true;
}

static bool isFrequencyValid(uint32_t freq) {
  if (freq >= FM_BAND_LOW && freq <= FM_BAND_HIGH) {
    return false;
  }
  if (filter == FILTER_VHF && freq >= bound) {
    return false;
  }
  if (filter == FILTER_UHF && freq < bound) {
    return false;
  }
  return true;
}

static void handleScanResult(VFOContext *ctx) {
  if (!BK4819_GetFrequencyScanResult(&currentFrequency)) {
    return;
  }

  gRedrawScreen = true;

  if (bandAutoSwitch) {
    filterSwitchCounter++;
  }

  if (isFrequencyValid(currentFrequency) &&
      DeltaF(currentFrequency, lastDetectedFrequency) < 300) {
    frequencyHits++;
  } else {
    frequencyHits = 1;
  }

  lastDetectedFrequency = currentFrequency;

  if (frequencyHits >= REQUIRED_FREQUENCY_HITS) {
    disableScan();
    RADIO_SetParam(ctx, PARAM_FREQUENCY, currentFrequency, false);
    RADIO_ApplySettings(ctx);
    frequencyHits = 0;
  } else {
    disableScan();
    enableScan();
  }
}

void FC_init(void) {
  bound = SETTINGS_GetFilterBound();
  BK4819_SelectFilterEx(filter);
  enableScan();
  frequencyHits = 0;
  filterSwitchCounter = 0;
  SCAN_SetMode(SCAN_MODE_NONE);
}

void FC_deinit(void) { disableScan(); }

void FC_update(void) {
  if (!CheckTimeout(&fcTimeuot)) {
    return;
  }

  ExtendedVFOContext *vfo = RADIO_GetCurrentVFO(gRadioState);
  VFOContext *ctx = &vfo->context;

  RADIO_UpdateMultiwatch(gRadioState);
  RADIO_CheckAndSaveVFO(gRadioState);

  if (isScanning) {
    handleScanResult(ctx);

    if (filterSwitchCounter >= FILTER_SWITCH_INTERVAL) {
      switchFilter();
    }

    SetTimeout(&fcTimeuot, 200 << gSettings.fcTime);
  } else {
    if (Now() - fcListenTimer >= SQL_DELAY) {
      RADIO_UpdateSquelch(gRadioState);
      if (vfo->is_open) {
        gRedrawScreen = true;
      } else {
        enableScan();
      }
      fcListenTimer = Now();
    }
  }
}

bool FC_key(KEY_Code_t key, Key_State_t state) {
  if (REGSMENU_Key(key, state)) {
    return true;
  }

  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (key == KEY_5) {
      hz = (hz == FC_HZ_LOW) ? FC_HZ_HIGH : FC_HZ_LOW;
    }
  }

  ExtendedVFOContext *vfo = RADIO_GetCurrentVFO(gRadioState);
  VFOContext *ctx = &vfo->context;

  if (state != KEY_RELEASED) {
    return false;
  }

  switch (key) {
  case KEY_1:
  case KEY_7:
    gSettings.fcTime = IncDecU(gSettings.fcTime, 0, 3 + 1, key == KEY_1);
    SETTINGS_DelayedSave();
    FC_init();
    break;
  case KEY_3:
  case KEY_9:
    RADIO_IncDecParam(ctx, PARAM_SQUELCH_VALUE, key == KEY_3, true);
    RADIO_ApplySettings(ctx);
    break;
  case KEY_STAR:
    return true;
  case KEY_SIDE1:
    LOOT_BlacklistLast();
    return true;
  case KEY_SIDE2:
    LOOT_WhitelistLast();
    return true;
  case KEY_F:
    switchFilter();
    return true;
  case KEY_6:
    bandAutoSwitch = !bandAutoSwitch;
    return true;
  case KEY_PTT:
    if (gLastActiveLoot) {
      FC_deinit();
      RADIO_SetParam(ctx, PARAM_FREQUENCY, gLastActiveLoot->f, true);
      APPS_run(APP_VFO1);
    }
    return true;
  default:
    break;
  }

  return false;
}

void FC_render(void) {
  ExtendedVFOContext *vfo = RADIO_GetCurrentVFO(gRadioState);
  VFOContext *ctx = &vfo->context;

  PrintMediumEx(0, 16, POS_L, C_FILL, "%s %ums HZ %u SQ %u %s",
                FILTER_NAMES[filter], 200 << gSettings.fcTime, hz,
                RADIO_GetParam(ctx, PARAM_SQUELCH_VALUE),
                bandAutoSwitch ? "[A]" : "");

  UI_BigFrequency(40, currentFrequency);

  if (gLastActiveLoot) {
    char string[16];
    UI_DrawLoot(gLastActiveLoot, LCD_WIDTH, 48, POS_R);
    if (gLastActiveLoot->code != 255) {
      DCS_CodeType_t type =
          gLastActiveLoot->isCd ? CODE_TYPE_DIGITAL : CODE_TYPE_CONTINUOUS_TONE;
      PrintRTXCode(string, type, gLastActiveLoot->code);
      PrintSmallEx(LCD_WIDTH, 40 + 8 + 6, POS_R, C_FILL, "%s", string);
    }
  }

  REGSMENU_Draw();
}
