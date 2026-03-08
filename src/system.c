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

static uint8_t DEAD_BUF[] = {0xDE, 0xAD};

static uint32_t secondTimer;
static uint32_t radioTimer;
static uint32_t toastTimer;
static uint32_t appsKeyboardTimer;

static uint32_t gFrameCount = 0;
static uint32_t gLastFpsUpdate = 0;
static uint16_t gCurrentFPS = 0;

// static uint32_t time_apps;

static void appRender() {
  if (!gRedrawScreen) {
    return;
  }

  /* if (Now() - gLastRender < 8) {
    return;
  } */

  gRedrawScreen = false;

  UI_ClearScreen();

  // uint32_t t = Now();
  APPS_render();
  // time_apps = Now() - t;
  if (gFInputActive) {
    FINPUT_render();
  }
  if (gKeymapActive) {
    KEYMAP_Render();
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

  STATUSLINE_render(); // coz of APPS_render calls STATUSLINE_SetText

  TOAST_Render();

  gLastRender = Now();

  // FPS counter
  /* gFrameCount++;
  if (Now() - gLastFpsUpdate >= 1000) { // Каждую секунду
    gCurrentFPS = gFrameCount;
    gFrameCount = 0;
    gLastFpsUpdate = Now();
  } */
  /* PrintSmallEx(LCD_WIDTH - 24, 4, POS_R, C_FILL, "FPS: %u", gCurrentFPS);
  PrintSmallEx(LCD_XCENTER, 32, POS_C, C_FILL, "A %u S %u T %u", time_apps,
               time_status, time_toast); */
  ST7565_Blit();
}

static void systemUpdate() {
  BATTERY_UpdateBatteryInfo();
  // BACKLIGHT_Update();
}

static void resetFull() {
  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, "0xFFing...");
  ST7565_Blit();

  PY25Q16_FullErase();

  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, "0xFFed!");
  ST7565_Blit();
  for (;;) {
  }
}

static void reset() {
  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, "Formatting...");
  ST7565_Blit();

  lfs_format(&gLfs, &gStorage.config);
  lfs_mount(&gLfs, &gStorage.config);

  UI_ClearScreen();
  PrintMediumEx(LCD_XCENTER, LCD_YCENTER, POS_C, C_FILL, "Release key 0!");
  ST7565_Blit();
  keyboard_tick_1ms();
  while (keyboard_is_pressed(KEY_0)) {
    SYSTICK_DelayMs(1);
    keyboard_tick_1ms();
  }
  NVIC_SystemReset();
}

static void loadSettingsOrReset() {
  if (!lfs_file_exists("Settings.set")) {
    STORAGE_INIT("Settings.set", Settings, 1);
    STORAGE_SAVE("Settings.set", 0, &gSettings);
  }
  STORAGE_LOAD("Settings.set", 0, &gSettings);

  if (!lfs_file_exists("Bands.bnd")) {
    STORAGE_INIT("Bands.bnd", Band, MAX_BANDS);
  }
  if (!lfs_file_exists("Channels.ch")) {
    STORAGE_INIT("Channels.ch", CH, 4096);
  }
}

static bool checkKeylock(KEY_State_t state, KEY_Code_t key) {
  bool isKeyLocked = gSettings.keylock;
  bool isPttLocked = gSettings.pttLock;
  bool isSpecialKey = key == KEY_PTT || key == KEY_SIDE1 || key == KEY_SIDE2;
  bool isLongPressF = state == KEY_LONG_PRESSED && key == KEY_F;

  if (isLongPressF) {
    gSettings.keylock = !gSettings.keylock;
    SETTINGS_Save();
    return true;
  }

  if (gSettings.keylock && state == KEY_LONG_PRESSED && key == KEY_8) {
    captureScreen();
    return true;
  }

  return isKeyLocked && (isPttLocked || !isSpecialKey) && !isLongPressF;
}

static bool keyAction(AppAction_t act) {
  switch (act.action) {
  case KA_FLASHLIGHT:
    BOARD_FlashlightToggle();
    return true;
  default:
    break;
  }

  return false;
}

static void onKey(KEY_Code_t key, KEY_State_t state) {
  BACKLIGHT_TurnOn();

  if (gCurrentApp != APP_SETTINGS && checkKeylock(state, key)) {
    gRedrawScreen = true;
    return;
  }

  if (gFInputActive && FINPUT_key(key, state)) {
    gRedrawScreen = true;
    gLastRender = 0;
  } else if (gTextInputActive && TEXTINPUT_key(key, state)) {
    gRedrawScreen = true;
    gLastRender = 0;
  } else if (gLootlistActive && LOOTLIST_key(key, state)) {
    gRedrawScreen = true;
    gLastRender = 0;
  } else if (gChlistActive && CHLIST_key(key, state)) {
    gRedrawScreen = true;
    gLastRender = 0;
  } else if (gKeymapActive && KEYMAP_Key(key, state)) {
    gRedrawScreen = true;
    gLastRender = 0;
  } else if (state == KEY_LONG_PRESSED && key == KEY_STAR) {
    KEYMAP_Show();
    gRedrawScreen = true;
    gLastRender = 0;
    return;
  } else if (state == KEY_LONG_PRESSED &&
             gCurrentKeymap.long_press[key].action != KA_NONE) {
    if (keyAction(gCurrentKeymap.long_press[key])) {
      gRedrawScreen = true;
      gLastRender = 0;
      return;
    }
  } else if (state == KEY_RELEASED &&
             gCurrentKeymap.click[key].action != KA_NONE) {
    if (keyAction(gCurrentKeymap.click[key])) {
      gRedrawScreen = true;
      gLastRender = 0;
      return;
    }
  } else if (APPS_key(key, state) || (MENU_IsActive() && key != KEY_EXIT)) {
    // LogC(LOG_C_BRIGHT_WHITE, "[SYS] Apps key %u %u", key, state);
    gRedrawScreen = true;
    gLastRender = 0;
  } else {
    // LogC(LOG_C_BRIGHT_WHITE, "[SYS] Global key %u %u", key, state);
    if (key == KEY_MENU) {
      if (state == KEY_LONG_PRESSED) {
        APPS_run(APP_SETTINGS);
      } else if (state == KEY_RELEASED) {
        APPS_run(APP_APPS_LIST);
      }
    }
    if (key == KEY_EXIT) {
      if (state == KEY_RELEASED) {
        APPS_exit();
      }
    }
  }
}

char dtmfBuf[16] = "\0";
uint8_t dtmfIdx = 0;
uint32_t lastDtmf;

bool checkInt() {
  if (BK4819_ReadRegister(0x0C) & 1) {
    BK4819_WriteRegister(0x02, 0x0000);
    uint16_t int_bits = BK4819_ReadRegister(0x02);

    if (int_bits & BK4819_REG_02_MASK_SQUELCH_LOST) {
      LogC(LOG_C_GREEN, "SQ -");
    }
    if (int_bits & BK4819_REG_02_MASK_SQUELCH_FOUND) {
      LogC(LOG_C_GREEN, "SQ +");
    }
    if (int_bits & BK4819_REG_02_MASK_FSK_RX_SYNC) {
      LogC(LOG_C_GREEN, "FSK RX Sync");
    }
    if (int_bits & BK4819_REG_02_MASK_FSK_FIFO_ALMOST_FULL) {
      LogC(LOG_C_GREEN, "FSK FIFO alm full");
    }
    if (int_bits & BK4819_REG_02_MASK_FSK_FIFO_ALMOST_EMPTY) {
      LogC(LOG_C_GREEN, "FSK FIFO alm empt");
    }
    if (int_bits & BK4819_REG_02_MASK_FSK_RX_FINISHED) {
      LogC(LOG_C_GREEN, "FSK RX finish");
    }
    if (int_bits & BK4819_REG_02_MASK_CxCSS_TAIL) {
      LogC(LOG_C_GREEN, "TAIL tone");
      // TOAST_Push("TAIL");
    }
    if (int_bits & BK4819_REG_02_MASK_CTCSS_FOUND) {
      LogC(LOG_C_GREEN, "CT +");
      uint32_t cd;
      uint16_t ct;
      BK4819_CssScanResult_t res = BK4819_GetCxCSSScanResult(&cd, &ct);
      TOAST_Push("CT:%u.%u", CTCSS_Options[ct] / 10, CTCSS_Options[ct] % 10);
    }
    if (int_bits & BK4819_REG_02_MASK_CTCSS_LOST) {
      LogC(LOG_C_GREEN, "CT -");
    }
    if (int_bits & BK4819_REG_02_MASK_CDCSS_FOUND) {
      LogC(LOG_C_GREEN, "CD +");
      TOAST_Push("CDCSS +");
    }
    if (int_bits & BK4819_REG_02_MASK_CDCSS_LOST) {
      LogC(LOG_C_GREEN, "CD -");
    }
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
      // TODO: process
      TOAST_Push("FSK: %04X %04X %04X %04x", FSK_RXDATA[0], FSK_RXDATA[1],
                 FSK_RXDATA[2], FSK_RXDATA[3]);
      gHasUnreadMessages = true;
      MESSENGER_update();
    }
    return true;
  }
  return false;
}

void SYS_Main() {
  LogC(LOG_C_BRIGHT_WHITE, "Keyboard init");
  keyboard_init(onKey);

  keyboard_tick_1ms();
  if (keyboard_is_pressed(KEY_EXIT)) {
    reset();
  } else if (keyboard_is_pressed(KEY_0)) {
    resetFull();
  } else {
    loadSettingsOrReset();

    LogC(LOG_C_BRIGHT_WHITE, "Bat init");
    BATTERY_UpdateBatteryInfo();

    // better UX
    STATUSLINE_render();
    ST7565_Blit();

    LogC(LOG_C_BRIGHT_WHITE, "Load bands");
    // BANDS_Load();

    LogC(LOG_C_BRIGHT_WHITE, "Run default app: %s",
         apps[gSettings.mainApp].name);
    APPS_run(gSettings.mainApp);
  }

  /* LogC(LOG_C_BRIGHT_WHITE, "USB MSC init");
  BOARD_USBInit(); */

  BACKLIGHT_TurnOn();
  LogC(LOG_C_BRIGHT_WHITE, "System initialized");

  for (;;) {
    SETTINGS_UpdateSave();

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
    /* if (gChlistActive) {
      CHLIST_update();
    } */

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

    // common: render 2 times per second minimum
    if (Now() - gLastRender >= 500) {
      BACKLIGHT_UpdateTimer();
      gRedrawScreen = true;
    }

    if (Now() - secondTimer >= 1000) {
      STATUSLINE_update();
      systemUpdate();
      secondTimer = Now();
    }

    appRender();

    if (SCAN_GetMode() == SCAN_MODE_SINGLE && checkInt()) {
      continue;
    }

    // __WFI();
  }
}
