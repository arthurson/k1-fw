#include "keymap.h"
#include "../driver/keyboard.h"
#include "../helper/keymap.h"
#include "../helper/menu.h"
#include "../helper/measurements.h"
#include "../settings.h"
#include "graphics.h"
#include "statusline.h"
#include <string.h>

bool gKeymapActive;

static const uint8_t ITEM_H = 19;

bool isSubmenu;
KEY_Code_t currentKey;
AppAction_t *currentAction;
uint8_t submenuScrollOffset = 0;

static Menu menu = {
    .itemHeight = ITEM_H,
};
static Menu submenu = {
    .itemHeight = MENU_ITEM_H,
};

static void renderItem(uint16_t index, uint8_t i) {
  uint8_t y = MENU_Y + i * ITEM_H;
  KeyAction clickAction = gCurrentKeymap.click[index].action;
  KeyAction longAction = gCurrentKeymap.long_press[index].action;
  
  // Truncate action names if too long for display
  char clickName[16], longName[16];
  strncpy(clickName, KA_NAMES[clickAction], sizeof(clickName) - 1);
  clickName[sizeof(clickName) - 1] = '\0';
  strncpy(longName, KA_NAMES[longAction], sizeof(longName) - 1);
  longName[sizeof(longName) - 1] = '\0';
  
  PrintMediumEx(13, y + 8, POS_L, C_INVERT, "%s: %s", KEY_NAMES[index],
                clickName);
  PrintMediumEx(13, y + 8 + 8, POS_L, C_INVERT, "%s L: %s", KEY_NAMES[index],
                longName);
}

static void renderSubItem(uint16_t index, uint8_t i) {
  uint8_t y = MENU_Y + i * MENU_ITEM_H;

  // Add scroll indicator if needed
  const char *actionName = KA_NAMES[index];
  char displayName[20];

  if (strlen(actionName) > 18) {
    strncpy(displayName, actionName, 17);
    displayName[17] = '~';
    displayName[18] = '\0';
  } else {
    strncpy(displayName, actionName, sizeof(displayName) - 1);
    displayName[sizeof(displayName) - 1] = '\0';
  }

  // Show selection marker
  if (index == submenu.i) {
    PrintMediumEx(13, y + 8, POS_L, C_INVERT, "> %s <", displayName);
  } else {
    PrintMediumEx(13, y + 8, POS_L, C_INVERT, "  %s", displayName);
  }

  // Show action category hint in status line
  if (index == submenu.i) {
    if (index >= KA_MODULATION && index <= KA_VOLUME) {
      STATUSLINE_SetText("Radio params");
    } else if (index >= KA_RSSI_GRAPH && index <= KA_PRO_MODE) {
      STATUSLINE_SetText("Display/UI");
    } else if (index >= KA_FREQ_INPUT && index <= KA_TUNE_TO_LOOT) {
      STATUSLINE_SetText("Freq/Ch");
    } else if (index >= KA_LOOTLIST && index <= KA_SAVE_LOOT_CH) {
      STATUSLINE_SetText("Scan/List");
    } else if (index >= KA_APP_VFO1 && index <= KA_EXIT_APP) {
      STATUSLINE_SetText("Apps");
    } else if (index >= KA_BAND_UP && index <= KA_RANGE_INPUT) {
      STATUSLINE_SetText("Band/Range");
    } else if (index >= KA_PTT && index <= KA_PTT) {
      STATUSLINE_SetText("PTT/TX");
    } else if (index >= KA_BL && index <= KA_INVERT_BTNS) {
      STATUSLINE_SetText("Settings");
    } else {
      STATUSLINE_SetText("UP/DOWN select, MENU ok");
    }
  }
}

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  if (state == KEY_PRESSED) {
    return false;
  }
  if (key == KEY_STAR) {
    return true;
  }
  if (key == KEY_UP || key == KEY_DOWN) {
    return false;
  }
  if (state == KEY_RELEASED) {
    if (key == KEY_EXIT) {
      if (isSubmenu) {
        MENU_Deinit();
        MENU_Init(&menu);
        return true;
      }
      KEYMAP_Hide();
      return true;
    }
  }

  if (key == KEY_MENU && state == KEY_RELEASED) {
    isSubmenu = true;
    currentKey = index;
    currentAction = &gCurrentKeymap.click[index];
    submenu.i = currentAction->action;
    if (submenu.i >= KA_COUNT) submenu.i = 0;
    MENU_Init(&submenu);
    return true;
  }

  if (key == KEY_MENU && state == KEY_LONG_PRESSED) {
    isSubmenu = true;
    currentKey = index;
    currentAction = &gCurrentKeymap.long_press[index];
    submenu.i = currentAction->action;
    if (submenu.i >= KA_COUNT) submenu.i = 0;
    MENU_Init(&submenu);
    return true;
  }

  menu.i = key;

  return true;
}
static bool subAction(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  // UP/DOWN уже обработаны в handleUpDownNavigation, обрабатываем только MENU/EXIT
  (void)index;
  
  // Only handle MENU and EXIT on release
  if (state != KEY_RELEASED) {
    return false;
  }

  if (key == KEY_EXIT) {
    MENU_Deinit();
    isSubmenu = false;
    MENU_Init(&menu);
    return true;
  }

  if (key == KEY_MENU) {
    currentAction->action = submenu.i;
    MENU_Deinit();
    isSubmenu = false;
    MENU_Init(&menu);
    return true;
  }

  return false;
}

void KEYMAP_Render() { MENU_Render(); }

bool KEYMAP_Key(KEY_Code_t key, KEY_State_t state) {
  return MENU_HandleInput(key, state);
}

void KEYMAP_Show() {
  menu.num_items = KEY_COUNT;
  menu.render_item = renderItem;
  menu.action = action;
  menu.items = NULL;

  submenu.num_items = KA_COUNT;
  submenu.render_item = renderSubItem;
  submenu.action = subAction;
  submenu.title = "Select Action";
  submenu.i = 0;
  submenu.x = 0;
  submenu.y = MENU_Y;
  submenu.width = LCD_WIDTH;
  submenu.height = LCD_HEIGHT - MENU_Y;
  submenu.items = NULL;

  gKeymapActive = true;
  MENU_Init(&menu);
}

void KEYMAP_Hide() {
  KEYMAP_Save();
  gKeymapActive = false;
  MENU_Deinit();
}
