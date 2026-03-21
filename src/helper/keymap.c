#include "keymap.h"
#include "../apps/apps.h"
#include "../driver/lfs.h"
#include "../external/printf/printf.h"
#include "storage.h"
#include <string.h>

AppKeymap_t gCurrentKeymap;

const char *KA_NAMES[] = {
    [KA_NONE] = "NONE",
    [KA_STEP] = "STEP",
    [KA_BW] = "BW",
    [KA_GAIN] = "GAIN",
    [KA_POWER] = "POWER",
    [KA_BL] = "BL",
    [KA_RSSI] = "RSSI",
    [KA_FLASHLIGHT] = "FLASHLIGHT",
    [KA_MONI] = "MONI",
    [KA_TX] = "TX",
    [KA_VOX] = "VOX",
    [KA_OFFSET] = "OFFSET",
    [KA_BLACKLIST_LAST] = "BL LAST",
    [KA_WHITELIST_LAST] = "WL LAST",
    [KA_FASTMENU1] = "FAST MENU1",
    [KA_FASTMENU2] = "FAST MENU2",
    [KA_CH_SETTING] = "CH SETTING",
    [KA_BANDS] = "BANDS",
    [KA_CHANNELS] = "CHANNELS",
    [KA_LOOTLIST] = "LOOTLIST",

    // Radio parameter actions
    [KA_MODULATION] = "MODULATION",
    [KA_SQUELCH_UP] = "SQL UP",
    [KA_SQUELCH_DOWN] = "SQL DOWN",
    [KA_OFFSET_UP] = "OFFSET UP",
    [KA_OFFSET_DOWN] = "OFFSET DOWN",
    [KA_OFFSET_DIR] = "OFFSET DIR",
    [KA_RADIO] = "RADIO",
    [KA_FILTER] = "FILTER",
    [KA_AFC] = "AFC",
    [KA_DEV] = "DEV",
    [KA_XTAL] = "XTAL",
    [KA_SCRAMBLER] = "SCRAMBLER",
    [KA_VOLUME] = "VOLUME",

    // Display & UI actions
    [KA_RSSI_GRAPH] = "RSSI GRAPH",
    [KA_LEVEL_DISPLAY] = "LEVEL DISP",
    [KA_ALWAYS_RSSI] = "ALWAYS RSSI",
    [KA_GRAPH_UNIT] = "GRAPH UNIT",
    [KA_VFO_MENU] = "VFO MENU",
    [KA_RADIO_SETTINGS] = "RadioRegs",
    [KA_PRO_MODE] = "PRO MODE",

    // Frequency & channel actions
    [KA_FREQ_INPUT] = "FREQ INPUT",
    [KA_CH_LIST] = "CH LIST",
    [KA_VFO_MODE] = "VFO MODE",
    [KA_NEXT_CH] = "NEXT CH",
    [KA_PREV_CH] = "PREV CH",
    [KA_NEXT_VFO] = "NEXT VFO",
    [KA_TUNE_TO_LOOT] = "TUNE LOOT",

    // Scan & list actions
    [KA_MULTIWATCH] = "MULTIWATCH",
    [KA_NEXT_BLACKLIST] = "NEXT BL",
    [KA_NEXT_WHITELIST] = "NEXT WL",
    [KA_CLEAR_LOOT] = "CLEAR LOOT",
    [KA_SAVE_LOOT_CH] = "SAVE LOOT",

    // Application control
    [KA_APP_VFO1] = "APP VFO1",
    [KA_APP_SCAN] = "APP SCAN",
    [KA_APP_FC] = "APP FC",
    [KA_APP_SETTINGS] = "APP SETTINGS",
    [KA_APP_FILES] = "APP FILES",
    [KA_APP_OSC] = "APP OSC",
    [KA_EXIT_APP] = "EXIT APP",

    // Band & range actions
    [KA_BAND_UP] = "BAND UP",
    [KA_BAND_DOWN] = "BAND DOWN",
    [KA_ZOOM_IN] = "ZOOM IN",
    [KA_ZOOM_OUT] = "ZOOM OUT",
    [KA_RANGE_INPUT] = "RANGE IN",

    // PTT & TX actions
    [KA_PTT] = "PTT",

    // Quick settings
    [KA_BL_MAX] = "BL MAX",
    [KA_BL_MIN] = "BL MIN",
    [KA_CONTRAST] = "CONTRAST",
    [KA_BEEP] = "BEEP",
    [KA_INVERT_BTNS] = "INV BUTTONS",
};

static char keymapDir[16];
static char keymapFile[32];

void KEYMAP_Load() {
  snprintf(keymapDir, 16, "/%s", apps[gCurrentApp].name);
  snprintf(keymapFile, 32, "%s/keymap.key", keymapDir);

  struct lfs_info info;
  if (lfs_stat(&gLfs, keymapDir, &info) < 0) {
    lfs_mkdir(&gLfs, keymapDir);
  }
  if (!lfs_file_exists(keymapFile)) {
    STORAGE_INIT(keymapFile, AppKeymap_t, 1);
  }

  STORAGE_LOAD(keymapFile, 1, &gCurrentKeymap);
}

void KEYMAP_Save() { STORAGE_SAVE(keymapFile, 1, &gCurrentKeymap); }
