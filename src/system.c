#include "system.h"
#include "apps/apps.h"
#include "apps/messenger.h"
#include "board.h"
#include "dcs.h"
#include "driver/backlight.h"
#include "driver/battery.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/keyboard.h"
#include "driver/lfs.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/CMSIS/Device/PY32F071/Include/py32f071xB.h"
#include "external/littlefs/lfs.h"
#include "external/printf/printf.h"
#include "helper/audio_io.h"
#include "helper/audio_rec.h"
#include "helper/bands.h"
#include "helper/fsk2.h"
#include "helper/keymap.h"
#include "helper/lootlist.h"
#include "helper/menu.h"
#include "helper/measurements.h"
#include "helper/scan.h"
#include "helper/screenshot.h"
#include "helper/storage.h"
#include "inc/channel.h"
#include "misc.h"
#include "settings.h"
#include "ui/chlist.h"
#include "ui/finput.h"
#include "ui/graphics.h"
#include "ui/keymap.h"
#include "ui/lootlist.h"
#include "ui/spectrum.h"
#include "ui/statusline.h"
#include "ui/textinput.h"
#include "ui/toast.h"
#include "helper/regs-menu.h"
#include "helper/vfomenu.h"
#include "helper/scan.h"
#include <string.h>

static uint32_t secondTimer;
static uint32_t toastTimer;
static uint32_t backlightTimer;
static uint32_t appsKeyboardTimer;

static void appRender(void) {
  if (!gRedrawScreen || Now() - gLastRender < 32) {
    return;
  }

  gRedrawScreen = false;
  UI_ClearScreen();
  APPS_render();

  if (gFInputActive) {
    FINPUT_render();
  }
  if (gTextInputActive) {
    TEXTINPUT_render();
  }
  if (gLootlistActive) {
    LOOTLIST_render();
  }
  if (gChlistActive) {
    CHLIST_render();
  }
  if (gKeymapActive) {
    KEYMAP_Render();
  }

  STATUSLINE_render();
  TOAST_Render();

  gLastRender = Now();
  ST7565_Blit();
}

static void showMsg(const char *msg) {
  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, msg);
  ST7565_Blit();
}

static void resetFull(void) {
  showMsg("0xFFing...");
  PY25Q16_FullErase();
  showMsg("0xFFed!");
  for (;;) {
  }
}

static void reset(void) {
  showMsg("Formatting...");
  lfs_format(&gLfs, &gStorage.config);
  lfs_mount(&gLfs, &gStorage.config);

  showMsg("Release key 0!");
  keyboard_tick_1ms();
  while (keyboard_is_pressed(KEY_0)) {
    SYSTICK_DelayMs(1);
    keyboard_tick_1ms();
  }
  NVIC_SystemReset();
}

static void loadSettingsOrReset(void) {
  if (!lfs_file_exists("Settings.set")) {
    STORAGE_INIT("Settings.set", Settings, 1);
    STORAGE_SAVE("Settings.set", 0, &gSettings);
  }
  STORAGE_LOAD("Settings.set", 0, &gSettings);

  if (!lfs_file_exists("Bands.bnd")) {
    STORAGE_INIT("Bands.bnd", Band, MAX_BANDS);
  }
}

static bool checkKeylock(KEY_State_t state, KEY_Code_t key) {
  if (state == KEY_LONG_PRESSED && key == KEY_F) {
    gSettings.keylock = !gSettings.keylock;
    SETTINGS_Save();
    return true;
  }
  if (gSettings.keylock && state == KEY_LONG_PRESSED && key == KEY_8) {
    captureScreen();
    return true;
  }

  bool isSpecialKey = key == KEY_PTT || key == KEY_SIDE1 || key == KEY_SIDE2;
  return gSettings.keylock && (gSettings.pttLock || !isSpecialKey);
}

static bool keyAction(AppAction_t act) {
  VFOContext *ctx = &RADIO_GetCurrentVFO(gRadioState)->context;

  switch (act.action) {
  // ========================================================================
  // Flashlight & Basic Actions
  // ========================================================================
  case KA_FLASHLIGHT:
    BOARD_FlashlightToggle();
    return true;

  // ========================================================================
  // Radio Parameter Actions
  // ========================================================================
  case KA_STEP:
    RADIO_IncDecParam(ctx, PARAM_STEP, true, true);
    return true;

  case KA_BW:
    RADIO_IncDecParam(ctx, PARAM_BANDWIDTH, true, true);
    return true;

  case KA_GAIN:
    RADIO_IncDecParam(ctx, PARAM_GAIN, true, true);
    return true;

  case KA_POWER:
    RADIO_IncDecParam(ctx, PARAM_POWER, true, true);
    return true;

  case KA_MODULATION:
    RADIO_IncDecParam(ctx, PARAM_MODULATION, true, true);
    return true;

  case KA_SQUELCH_UP:
    RADIO_IncDecParam(ctx, PARAM_SQUELCH_VALUE, true, true);
    return true;

  case KA_SQUELCH_DOWN:
    RADIO_IncDecParam(ctx, PARAM_SQUELCH_VALUE, false, true);
    return true;

  case KA_OFFSET_UP:
    RADIO_IncDecParam(ctx, PARAM_TX_OFFSET, true, true);
    return true;

  case KA_OFFSET_DOWN:
    RADIO_IncDecParam(ctx, PARAM_TX_OFFSET, false, true);
    return true;

  case KA_OFFSET_DIR:
    RADIO_IncDecParam(ctx, PARAM_TX_OFFSET_DIR, true, true);
    return true;

  case KA_RADIO:
    RADIO_IncDecParam(ctx, PARAM_RADIO, true, true);
    return true;

  case KA_FILTER:
    RADIO_IncDecParam(ctx, PARAM_FILTER, true, true);
    return true;

  case KA_AFC:
    RADIO_IncDecParam(ctx, PARAM_AFC, true, true);
    return true;

  case KA_DEV:
    RADIO_IncDecParam(ctx, PARAM_DEV, true, true);
    return true;

  case KA_XTAL:
    RADIO_IncDecParam(ctx, PARAM_XTAL, true, true);
    return true;

  case KA_SCRAMBLER:
    RADIO_IncDecParam(ctx, PARAM_SCRAMBLER, true, true);
    return true;

  case KA_VOLUME:
    RADIO_IncDecParam(ctx, PARAM_VOLUME, true, true);
    return true;

  // ========================================================================
  // Display & UI Actions
  // ========================================================================
  case KA_RSSI:
    gShowAllRSSI = !gShowAllRSSI;
    return true;

  case KA_RSSI_GRAPH:
    gSettings.showLevelInVFO = !gSettings.showLevelInVFO;
    SETTINGS_DelayedSave();
    return true;

  case KA_ALWAYS_RSSI:
    gSettings.alwaysRssi = !gSettings.alwaysRssi;
    SETTINGS_DelayedSave();
    return true;

  case KA_GRAPH_UNIT:
    SP_NextGraphUnit(true);
    return true;

  case KA_LEVEL_DISPLAY:
    gSettings.showLevelInVFO = !gSettings.showLevelInVFO;
    SETTINGS_DelayedSave();
    return true;

  case KA_VFO_MENU:
    VFOMENU_Key(KEY_F, KEY_RELEASED);
    return true;

  case KA_RADIO_SETTINGS:
    REGSMENU_Key(KEY_0, KEY_RELEASED);
    return true;

  case KA_PRO_MODE:
    gSettings.iAmPro = !gSettings.iAmPro;
    SETTINGS_Save();
    return true;

  // ========================================================================
  // Monitor & TX Actions
  // ========================================================================
  case KA_MONI:
    gMonitorMode = !gMonitorMode;
    return true;

  case KA_TX:
    RADIO_ToggleTX(ctx, true);
    return true;

  case KA_PTT:
    RADIO_ToggleTX(ctx, !ctx->tx_state.is_active);
    return true;

  case KA_VOX:
    // VOX toggle - implement if needed
    return true;

  // ========================================================================
  // Frequency & Channel Actions
  // ========================================================================
  case KA_FREQ_INPUT:
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(NULL);
    return true;

  case KA_VFO_MODE: {
    uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);
    RADIO_SaveCurrentVFO(gRadioState);
    RADIO_ToggleVFOMode(gRadioState, vfoN);
    return true;
  }

  case KA_NEXT_CH:
    RADIO_NextChannel(true);
    return true;

  case KA_PREV_CH:
    RADIO_NextChannel(false);
    return true;

  case KA_NEXT_VFO: {
    uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);
    RADIO_SaveCurrentVFO(gRadioState);
    RADIO_SwitchVFO(gRadioState, IncDecU(vfoN, 0, gRadioState->num_vfos, true));
    return true;
  }

  case KA_TUNE_TO_LOOT:
    if (gLastActiveLoot) {
      RADIO_SetParam(ctx, PARAM_FREQUENCY, gLastActiveLoot->f, true);
      RADIO_ApplySettings(ctx);
    }
    return true;

  // ========================================================================
  // Scan & List Actions
  // ========================================================================
  case KA_LOOTLIST:
    LOOTLIST_init();
    gLootlistActive = true;
    return true;

  case KA_CH_LIST:
    CHLIST_init();
    gChlistActive = true;
    return true;

  case KA_MULTIWATCH:
    RADIO_ToggleMultiwatch(gRadioState, !gRadioState->multiwatch_enabled);
    return true;

  case KA_BLACKLIST_LAST:
    LOOT_BlacklistLast();
    return true;

  case KA_WHITELIST_LAST:
    LOOT_WhitelistLast();
    return true;

  case KA_NEXT_BLACKLIST:
    SCAN_NextBlacklist();
    return true;

  case KA_NEXT_WHITELIST:
    SCAN_NextWhitelist();
    return true;

  case KA_CLEAR_LOOT:
    LOOT_Clear();
    return true;

  case KA_SAVE_LOOT_CH:
    // Save loot to channels - implement if needed
    return true;

  // ========================================================================
  // Band & Range Actions
  // ========================================================================
  case KA_BANDS:
    // Open bands menu - implement if needed
    return true;

  case KA_CHANNELS:
    // Open channels menu - implement if needed
    return true;

  case KA_BAND_UP:
  case KA_BAND_DOWN:
    // Band shift - scanner specific
    return true;

  case KA_ZOOM_IN:
  case KA_ZOOM_OUT:
    // Range zoom - scanner specific
    return true;

  case KA_RANGE_INPUT:
    // Range input - scanner specific
    return true;

  // ========================================================================
  // Application Control
  // ========================================================================
  case KA_APP_VFO1:
    APPS_run(APP_VFO1);
    return true;

  case KA_APP_SCAN:
    APPS_run(APP_SCANER);
    return true;

  case KA_APP_FC:
    APPS_run(APP_FC);
    return true;

  case KA_APP_SETTINGS:
    APPS_run(APP_SETTINGS);
    return true;

  case KA_APP_FILES:
    APPS_run(APP_FILES);
    return true;

  /* case KA_APP_OSC:
    APPS_run(APP_OSC);
    return true; */

  case KA_EXIT_APP:
    APPS_exit();
    return true;

  // ========================================================================
  // Fast Menu & Settings
  // ========================================================================
  case KA_FASTMENU1:
  case KA_FASTMENU2:
    // Fast menu slots - implement if needed
    return true;

  case KA_CH_SETTING:
    APPS_run(APP_SETTINGS);
    return true;

  case KA_BL:
  case KA_BL_MAX:
  case KA_BL_MIN:
  case KA_CONTRAST:
  case KA_BEEP:
  case KA_INVERT_BTNS:
    // Settings shortcuts - implement if needed
    return true;

  case KA_NONE:
  default:
    return false;
  }
}

#define HANDLE_OVERLAY(active, fn, k, s)                                       \
  if (active && fn(k, s)) {                                                    \
    gRedrawScreen = true;                                                      \
    gLastRender = 0;                                                           \
    return;                                                                    \
  }

static void onKey(KEY_Code_t key, KEY_State_t state) {
  BACKLIGHT_TurnOn();

  if (gCurrentApp != APP_SETTINGS && checkKeylock(state, key)) {
    gRedrawScreen = true;
    return;
  }

  HANDLE_OVERLAY(gFInputActive, FINPUT_key, key, state)
  HANDLE_OVERLAY(gTextInputActive, TEXTINPUT_key, key, state)
  HANDLE_OVERLAY(gLootlistActive, LOOTLIST_key, key, state)
  HANDLE_OVERLAY(gChlistActive, CHLIST_key, key, state)
  HANDLE_OVERLAY(gKeymapActive, KEYMAP_Key, key, state)

  if (state == KEY_LONG_PRESSED && key == KEY_STAR) {
    KEYMAP_Show();
  } else if (state == KEY_LONG_PRESSED &&
             gCurrentKeymap.long_press[key].action != KA_NONE) {
    if (!keyAction(gCurrentKeymap.long_press[key])) {
      goto apps;
    }
  } else if (state == KEY_RELEASED &&
             gCurrentKeymap.click[key].action != KA_NONE) {
    if (!keyAction(gCurrentKeymap.click[key])) {
      goto apps;
    }
  } else {
  apps:
    if (APPS_key(key, state) || (MENU_IsActive() && key != KEY_EXIT)) {
    } else if (key == KEY_MENU) {
      if (state == KEY_LONG_PRESSED) {
        APPS_run(APP_SETTINGS);
      } else if (state == KEY_RELEASED) {
        APPS_run(APP_APPS_LIST);
      }
    } else if (key == KEY_EXIT && state == KEY_RELEASED) {
      APPS_exit();
    }
  }

  gRedrawScreen = true;
  gLastRender = 0;
}

static char dtmfBuf[16] = "\0";
static uint8_t dtmfIdx = 0;
static uint32_t lastDtmf;

static bool checkInt(void) {
  if (!(BK4819_ReadRegister(0x0C) & 1)) {
    return false;
  }

  BK4819_WriteRegister(0x02, 0x0000);
  uint16_t int_bits = BK4819_ReadRegister(0x02);

  SCAN_HandleInterrupt(int_bits);

  if (int_bits & BK4819_REG_02_MASK_DTMF_5TONE_FOUND) {
    const char c = DTMF_GetCharacter(BK4819_GetDTMF_5TONE_Code());
    if (dtmfIdx < ARRAY_SIZE(dtmfBuf) - 1) {
      dtmfBuf[dtmfIdx++] = c;
      dtmfBuf[dtmfIdx] = '\0';
      lastDtmf = Now();
    }
    LogC(LOG_C_GREEN, "DTMF %c", c);
  }

  if (RF_FskReceive(int_bits)) {
    TOAST_Push("FSK: %04X %04X %04X %04x", FSK_RXDATA[0], FSK_RXDATA[1],
               FSK_RXDATA[2], FSK_RXDATA[3]);
    gHasUnreadMessages = true;
    MESSENGER_update();
  }

  return true;
}

void SYS_Main(void) {
  LogC(LOG_C_BRIGHT_WHITE, "Keyboard init");
  keyboard_init(onKey);
  keyboard_tick_1ms();

  if (keyboard_is_pressed(KEY_EXIT)) {
    reset();
  } else if (keyboard_is_pressed(KEY_0)) {
    resetFull();
  } else {
    loadSettingsOrReset();
    BATTERY_UpdateBatteryInfo();
    STATUSLINE_render();
    ST7565_Blit();
    LogC(LOG_C_BRIGHT_WHITE, "Run: %s", apps[gSettings.mainApp].name);
    APPS_run(gSettings.mainApp);
  }

  BACKLIGHT_TurnOn();
  LogC(LOG_C_BRIGHT_WHITE, "System initialized");

  for (;;) {
    SETTINGS_UpdateSave();
    checkInt();
    SCAN_Check();

    if (dtmfIdx > 0 && Now() - lastDtmf > 400) {
      TOAST_Push("DTMF: %s", dtmfBuf);
      dtmfIdx = 0;
    }

    if (gFInputActive) {
      FINPUT_update();
    }
    if (gTextInputActive) {
      TEXTINPUT_update();
    }
    if (gLootlistActive) {
      LOOTLIST_update();
    }

    AUDIO_IO_Update();
    APPS_update();

    if (Now() - toastTimer >= 40) {
      TOAST_Update();
      toastTimer = Now();
    }
    if (Now() - appsKeyboardTimer >= 1) {
      keyboard_tick_1ms();
      appsKeyboardTimer = Now();
    }
    if (Now() - backlightTimer >= 500) {
      BACKLIGHT_UpdateTimer();
      backlightTimer = Now();
    }
    if (Now() - secondTimer >= 1000) {
      BATTERY_UpdateBatteryInfo();
      STATUSLINE_update();
      secondTimer = Now();
    }

    if (Now() - gLastRender >= 500) {
      gRedrawScreen = true;
    }

    appRender();
  }
}
