#include "chlist.h"
#include "../apps/apps.h"
#include "../apps/vfo1.h"
#include "../driver/uart.h"
#include "../helper/bands.h"
#include "../helper/menu.h"
#include "../helper/storage.h"
#include "../radio.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "textinput.h"
#include <string.h>

typedef enum {
  MODE_INFO,
  MODE_TX,
  MODE_SCANLIST,
  MODE_SCANLIST_SELECT,
  MODE_DELETE,
  // MODE_TYPE,
  // MODE_SELECT,
} CHLIST_ViewMode;

static char *VIEW_MODE_NAMES[] = {
    "INFO",   //
    "TX",     //
    "SL",     //
    "SL SEL", //
    "DEL",    //
              // "TYPE",     //
              // "CH SEL",   //
};

bool gChlistActive;

bool gChSaveMode = false;

static uint8_t viewMode = MODE_INFO;

static CH ch;
static char tempName[10] = {0};

static void renderItem(uint16_t index, uint8_t i) {
  uint16_t chNum = index;

  // Clear channel data before loading to prevent stale data display
  memset(&ch, 0, sizeof(CH));
  STORAGE_LOAD("Channels.ch", chNum, &ch);

  uint8_t y = MENU_Y + i * MENU_ITEM_H;

  if (IsReadable(ch.name)) {
    PrintMediumEx(13, y + 8, POS_L, C_INVERT, "%s", ch.name);
  } else {
    PrintMediumEx(13, y + 8, POS_L, C_INVERT, "CH-%u", chNum);
    return;
  }

  switch (viewMode) {
  case MODE_INFO:
    PrintSmallEx(LCD_WIDTH - 5, y + 8, POS_R, C_INVERT, "%u.%03u %u.%03u",
                 ch.rxF / MHZ, ch.rxF / 100 % 1000, ch.txF / MHZ,
                 ch.txF / 100 % 1000);
    break;
  case MODE_SCANLIST:
    UI_Scanlists(LCD_WIDTH - 32, y + 3, ch.scanlists);
    break;
  case MODE_TX:
    PrintSmallEx(LCD_WIDTH - 5, y + 7, POS_R, C_INVERT, "%s",
                 ch.allowTx ? "ON" : "OFF");
    break;
  }
}

static uint16_t channelIndex;

/* static void save() {
  gChEd.scanlists = 0;
  CHANNELS_Save(getChannelNumber(channelIndex), &gChEd);
  // RADIO_LoadCurrentVFO();
  memset(gChEd.name, 0, sizeof(gChEd.name));
  APPS_exit();
  APPS_exit();
} */

/* static void saveNamed() {
  strcpy(gChEd.name, gTextinputText);
  save();
} */

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  uint16_t chNum = index;
  /* if (viewMode == MODE_SCANLIST || viewMode == MODE_SCANLIST_SELECT) {
    if ((state == KEY_LONG_PRESSED || state == KEY_RELEASED) &&
        (key > KEY_0 && key < KEY_9)) {
      if (viewMode == MODE_SCANLIST_SELECT) {
        CHANNELS_SelectScanlistByKey(key, state == KEY_LONG_PRESSED);
        CHLIST_init();
      } else {
        CHANNELS_Load(chNum, &ch);
        ch.scanlists = CHANNELS_ScanlistByKey(ch.scanlists, key,
                                              state == KEY_LONG_PRESSED);
        CHANNELS_Save(chNum, &ch);
      }
      return true;
    }
  } */
  CH tmp = {0}; // Initialize to zero to prevent garbage data
  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      MENU_Deinit();
      gChlistActive = false;
      return true;
    case KEY_1:
      if (viewMode == MODE_DELETE) {
        // Try to load existing channel, or use empty struct
        bool loaded = STORAGE_LOAD("Channels.ch", chNum, &tmp);
        tmp.name[0] = '\0'; // Clear name to mark as empty
        bool saved = STORAGE_SAVE("Channels.ch", chNum, &tmp);
        Log("[CHLIST] Delete CH %u: loaded=%d, saved=%d", chNum, loaded, saved);
        return true;
      }

      if (viewMode == MODE_TX) {
        if (STORAGE_LOAD("Channels.ch", chNum, &tmp)) {
          tmp.allowTx = !tmp.allowTx;
          STORAGE_SAVE("Channels.ch", chNum, &tmp);
        }
        return true;
      }
      break;
    case KEY_PTT:
      RADIO_LoadChannelToVFO(gRadioState,
                             RADIO_GetCurrentVFONumber(gRadioState), chNum);
      APPS_run(APP_VFO1);
      return true;
    /* case KEY_F:
      gChNum = chNum;
      CHANNELS_Load(gChNum, &gChEd);
      APPS_run(APP_CH_CFG);
      return true; */
    /* case KEY_MENU:
      if (gChSaveMode) {
        CHANNELS_LoadScanlist(gChListFilter, gSettings.currentScanlist);

        channelIndex = index;
        if (gChEd.name[0] == '\0') {
          gTextinputText = tempName;
          sprintf(gTextinputText, "%lu.%05lu", gChEd.rxF / MHZ,
                  gChEd.rxF % MHZ);
          gTextInputSize = 9;
          gTextInputCallback = saveNamed;
          APPS_run(APP_TEXTINPUT);
        } else {
          save();
        }
        return true;
      }
      LogC(LOG_C_YELLOW, "BAND Selected by user");
      BANDS_Select(chNum, true);
      APPS_exit();
      return true; */
    default:
      return false;
    }
  }
  return false;
}

static Menu chListMenu = {
    .render_item = renderItem, .itemHeight = MENU_ITEM_H, .action = action};

void CHLIST_init() {
  /* if (gChSaveMode) {
    gChListFilter = 1 << gChEd.meta.type | (1 << TYPE_EMPTY);
  }
  CHANNELS_LoadScanlist(gChListFilter, gSettings.currentScanlist);
  Log("Scanlist loaded: size=%u", gScanlistSize); */

  /* for (uint16_t i = 0; i < gScanlistSize; i++) {
    Log("gScanlist[%u] = %u", i, gScanlist[i]);
  } */

  // chListMenu.num_items = gScanlistSize;
  chListMenu.num_items = 4096;
  MENU_Init(&chListMenu);
  // TODO: set menu index
  /* if (gChListFilter == TYPE_FILTER_BAND ||
      gChListFilter == TYPE_FILTER_BAND_SAVE) {
    channelIndex = BANDS_GetScanlistIndex();
  }
  if (gScanlistSize == 0) {
    channelIndex = 0;
  } else if (channelIndex > gScanlistSize) {
    channelIndex = gScanlistSize - 1;
  } */
}

void CHLIST_deinit() { gChSaveMode = false; }

bool CHLIST_key(KEY_Code_t key, Key_State_t state) {
  /* if (state == KEY_LONG_PRESSED) {
    switch (key) {
    case KEY_0:
      gSettings.currentScanlist = 0;
      SETTINGS_Save();
      CHANNELS_LoadScanlist(TYPE_FILTER_CH, gSettings.currentScanlist);
      CHLIST_init();
      break;
    default:
      break;
    }
  } */
  if (state == KEY_RELEASED) {
    switch (key) {
    /* case KEY_0:
      switch (gChListFilter) {
      case TYPE_FILTER_CH:
        gChListFilter = TYPE_FILTER_BAND;
        break;
      case TYPE_FILTER_BAND:
        gChListFilter = TYPE_FILTER_VFO;
        break;
      case TYPE_FILTER_VFO:
        gChListFilter = TYPE_FILTER_CH;
        break;
      case TYPE_FILTER_CH_SAVE:
        gChListFilter = TYPE_FILTER_BAND_SAVE;
        break;
      case TYPE_FILTER_BAND_SAVE:
        gChListFilter = TYPE_FILTER_VFO_SAVE;
        break;
      case TYPE_FILTER_VFO_SAVE:
        gChListFilter = TYPE_FILTER_CH_SAVE;
        break;
      }
      CHANNELS_LoadScanlist(gChListFilter, gSettings.currentScanlist);
      chListMenu.num_items = gScanlistSize;
      MENU_Init(&chListMenu);

      return true; */
    case KEY_STAR:
      viewMode = IncDecU(viewMode, 0, ARRAY_SIZE(VIEW_MODE_NAMES), true);
      return true;
    default:
      break;
    }
  }

  if (MENU_HandleInput(key, state)) {
    return true;
  }

  return false;
}

void CHLIST_render() {
  MENU_Render();
  STATUSLINE_SetText("%s", VIEW_MODE_NAMES[viewMode]);
}
