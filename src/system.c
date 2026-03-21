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
#include "helper/menu.h"
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
#include "ui/statusline.h"
#include "ui/textinput.h"
#include "ui/toast.h"
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
  if (act.action == KA_FLASHLIGHT) {
    BOARD_FlashlightToggle();
    return true;
  }
  return false;
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
