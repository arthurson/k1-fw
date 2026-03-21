#ifndef KEYMAP_H
#define KEYMAP_H

#include "../driver/keyboard.h"
#include <stdint.h>

typedef enum {
  KA_NONE,
  KA_STEP,
  KA_BW,
  KA_GAIN,
  KA_POWER,
  KA_BL,
  KA_RSSI,
  KA_FLASHLIGHT,
  KA_MONI,
  KA_TX,
  KA_VOX,
  KA_OFFSET,
  KA_BLACKLIST_LAST,
  KA_WHITELIST_LAST,
  KA_FASTMENU1,
  KA_FASTMENU2,
  KA_CH_SETTING,
  KA_BANDS,
  KA_CHANNELS,
  KA_LOOTLIST,

  // Radio parameter actions
  KA_MODULATION,      // Toggle modulation type
  KA_SQUELCH_UP,      // Increase squelch value
  KA_SQUELCH_DOWN,    // Decrease squelch value
  KA_OFFSET_UP,       // Increase TX offset
  KA_OFFSET_DOWN,     // Decrease TX offset
  KA_OFFSET_DIR,      // Toggle offset direction
  KA_RADIO,           // Toggle radio type
  KA_FILTER,          // Toggle filter (VHF/UHF)
  KA_AFC,             // Toggle AFC
  KA_DEV,             // Toggle deviation
  KA_XTAL,            // Toggle XTAL
  KA_SCRAMBLER,       // Toggle scrambler
  KA_VOLUME,          // Toggle volume (SI4732)

  // Display & UI actions
  KA_RSSI_GRAPH,      // Toggle RSSI graph
  KA_LEVEL_DISPLAY,   // Toggle level in VFO
  KA_ALWAYS_RSSI,     // Toggle always RSSI bar
  KA_GRAPH_UNIT,      // Next graph measurement
  KA_VFO_MENU,        // Toggle VFO menu
  KA_RADIO_SETTINGS,  // Toggle radio settings menu
  KA_PRO_MODE,        // Toggle PRO mode

  // Frequency & channel actions
  KA_FREQ_INPUT,      // Show frequency input
  KA_CH_LIST,         // Show channel list
  KA_VFO_MODE,        // Toggle VFO mode (F<->CH)
  KA_NEXT_CH,         // Next channel
  KA_PREV_CH,         // Previous channel
  KA_NEXT_VFO,        // Next VFO
  KA_TUNE_TO_LOOT,    // Tune to last loot

  // Scan & list actions
  KA_MULTIWATCH,      // Toggle multiwatch
  KA_NEXT_BLACKLIST,  // Skip to next blacklisted
  KA_NEXT_WHITELIST,  // Skip to next whitelisted
  KA_CLEAR_LOOT,      // Clear all loot
  KA_SAVE_LOOT_CH,    // Save loot to channels

  // Application control
  KA_APP_VFO1,        // Run VFO app
  KA_APP_SCAN,        // Run Scanner app
  KA_APP_FC,          // Run FC app
  KA_APP_SETTINGS,    // Run Settings app
  KA_APP_FILES,       // Run Files app
  KA_APP_OSC,         // Run Oscilloscope app
  KA_EXIT_APP,        // Exit current app

  // Band & range actions
  KA_BAND_UP,         // Shift band up
  KA_BAND_DOWN,       // Shift band down
  KA_ZOOM_IN,         // Zoom frequency range in
  KA_ZOOM_OUT,        // Zoom frequency range out
  KA_RANGE_INPUT,     // Set frequency range

  // PTT & TX actions
  KA_PTT,             // Toggle PTT

  // Quick settings
  KA_BL_MAX,          // Backlight max brightness
  KA_BL_MIN,          // Backlight min brightness
  KA_CONTRAST,        // LCD contrast
  KA_BEEP,            // Toggle beep
  KA_INVERT_BTNS,     // Invert buttons

  KA_COUNT,
} KeyAction;

typedef struct {
  KeyAction action;
  uint8_t param; // Параметр действия (например, ID приложения)
} AppAction_t;

typedef struct {
  AppAction_t click[KEY_COUNT]; // Действия на клик (KEY_RELEASED)
  AppAction_t long_press[KEY_COUNT]; // Действия на удержание (KEY_LONG_PRESSED)
} AppKeymap_t;

void KEYMAP_Load();
void KEYMAP_Save();

extern AppKeymap_t gCurrentKeymap;
extern const char *KA_NAMES[];

#endif // !KEYMAP_H
