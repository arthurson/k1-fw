#include "../ui/rangelist.h"
#include "../apps/apps.h"
#include "../driver/bk4829.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/bands.h"
#include "../helper/menu.h"
#include "../helper/storage.h"
#include "../radio.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool gRangelistActive;
static uint8_t menuIndex = 0;
static const uint8_t RANGE_MENU_ITEM_H = 8;
static void initMenu();

// Edit mode state
typedef enum {
  REDIT_MODE_NONE,
  REDIT_MODE_ACTIVE,
  REDIT_MODE_FINPUT,
} RangeEditMode;

static RangeEditMode gRangeEditMode = REDIT_MODE_NONE;
static uint16_t gEditRangeIndex = 0;
static uint8_t gEditRangeField = 0;

// Fields: 0=Name, 1=Start, 2=End, 3=Step, 4=Modulation, 5=BW, 6=Radio, 7=SQL, 8=Gain, 9=TX
#define REDIT_FIELD_COUNT 10
static const char *REDIT_FIELD_NAMES[] = {
    "Name",
    "Start Freq",
    "End Freq",
    "Step",
    "Modulation",
    "Bandwidth",
    "Radio",
    "Squelch",
    "Gain",
    "TX Allow",
};

static void renderItem(uint16_t index, uint8_t i) {
  const RangeEntry *item = RLST_Item(index);
  const uint8_t y = MENU_Y + i * RANGE_MENU_ITEM_H + 7;

  PrintMediumEx(2, y, POS_L, C_FILL, "%u:%s", index + 1, item->name);
  PrintSmallEx(40, y, POS_L, C_INVERT, "%lu-%lu", item->start / KHZ,
               item->end / KHZ);
  PrintSmallEx(LCD_WIDTH - 2, y, POS_R, C_INVERT, "%s",
               item->allowTx ? "TX" : "RX");
}

static void tuneToRange(const RangeEntry *rng, bool save) {
  Band band = RLST_ToBand(rng);
  BANDS_RangeClear();
  BANDS_RangePush(band);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, rng->start, save);
  RADIO_SetParam(ctx, PARAM_STEP, rng->step, true);
  RADIO_SetParam(ctx, PARAM_MODULATION, rng->modulation, true);
  RADIO_SetParam(ctx, PARAM_BANDWIDTH, rng->bw, true);
  RADIO_SetParam(ctx, PARAM_RADIO, rng->radio, true);
  RADIO_SetParam(ctx, PARAM_SQUELCH_TYPE, rng->squelch.type, true);
  RADIO_SetParam(ctx, PARAM_SQUELCH_VALUE, rng->squelch.value, true);
  RADIO_SetParam(ctx, PARAM_GAIN, rng->gainIndex, true);
  RADIO_ApplySettings(ctx);
}

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  RangeEntry *rng = RLST_Item(index);

  VFOContext *ctx = &RADIO_GetCurrentVFO(gRadioState)->context;

  if (state == KEY_LONG_PRESSED) {
    switch (key) {
    case KEY_0:
      RLST_Clear();
      initMenu();
      return true;
    case KEY_F:
      // Save ranges
      if (RLST_Save()) {
        STATUSLINE_SetText("Ranges saved");
      } else {
        STATUSLINE_SetText("Save failed!");
      }
      return true;
    default:
      break;
    }
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      MENU_Deinit();
      gRangelistActive = false;
      return true;
    case KEY_UP:
    case KEY_DOWN:
      tuneToRange(rng, false);
      return true;
    case KEY_MENU:
      tuneToRange(rng, true);
      APPS_exit();
      return true;
    case KEY_0:
      RLST_Remove(menuIndex);
      if (menuIndex > RLST_Size() - 1) {
        menuIndex = RLST_Size() - 1;
      }
      initMenu();
      return true;
    case KEY_1:
      // Add new range
      RLST_Add(14400000, 14600000, "NewRange");
      initMenu();
      return true;
    case KEY_6:
      // Enter edit mode
      gEditRangeIndex = index;
      gEditRangeField = 0;
      gRangeEditMode = REDIT_MODE_ACTIVE;
      return true;
    case KEY_9:
      // Load ranges
      if (RLST_Load()) {
        STATUSLINE_SetText("Ranges loaded");
        initMenu();
      } else {
        STATUSLINE_SetText("Load failed!");
      }
      return true;
    default:
      break;
    }
  }
  return false;
}

// Edit mode callbacks
static void cbSetRangeStart(uint32_t f, uint32_t _) {
  (void)_;
  RangeEntry *rng = RLST_Item(gEditRangeIndex);
  rng->start = f;
  gRangeEditMode = REDIT_MODE_ACTIVE;
  gFInputActive = false;
  STATUSLINE_SetText("Start: %lu.%05lu", f / MHZ, f % MHZ);
}

static void cbSetRangeEnd(uint32_t f, uint32_t _) {
  (void)_;
  RangeEntry *rng = RLST_Item(gEditRangeIndex);
  rng->end = f;
  gRangeEditMode = REDIT_MODE_ACTIVE;
  gFInputActive = false;
  STATUSLINE_SetText("End: %lu.%05lu", f / MHZ, f % MHZ);
}

static void cbSetRangeStep(uint32_t step, uint32_t _) {
  (void)_;
  RangeEntry *rng = RLST_Item(gEditRangeIndex);
  rng->step = (Step)step;
  gRangeEditMode = REDIT_MODE_ACTIVE;
  gFInputActive = false;
  STATUSLINE_SetText("Step: %lu", step);
}

static void editRangeField(uint16_t index, uint8_t field) {
  RangeEntry *rng = RLST_Item(index);

  switch (field) {
  case 0: // Name - cycle through simple names
    // For simplicity, just change first character
    rng->name[0] = (rng->name[0] >= 'Z') ? 'A' : rng->name[0] + 1;
    STATUSLINE_SetText("Name: %s", rng->name);
    break;
  case 1: // Start frequency
    gFInputCallback = cbSetRangeStart;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    gFInputValue1 = rng->start;
    gFInputValue2 = 0;
    FINPUT_init();
    gRangeEditMode = REDIT_MODE_FINPUT;
    break;
  case 2: // End frequency
    gFInputCallback = cbSetRangeEnd;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    gFInputValue1 = rng->end;
    gFInputValue2 = 0;
    FINPUT_init();
    gRangeEditMode = REDIT_MODE_FINPUT;
    break;
  case 3: // Step
    gFInputCallback = cbSetRangeStep;
    FINPUT_setup(0, 20, UNIT_KHZ, false);
    gFInputValue1 = rng->step;
    FINPUT_init();
    gRangeEditMode = REDIT_MODE_FINPUT;
    break;
  case 4: // Modulation
    rng->modulation = (rng->modulation + 1) % 5;
    STATUSLINE_SetText(
        "Mod: %s",
        rng->modulation == 0   ? "FM"
        : rng->modulation == 1 ? "AM"
        : rng->modulation == 2 ? "WFM"
        : rng->modulation == 3 ? "USB"
                                 : "LSB");
    break;
  case 5: // Bandwidth
    rng->bw = (rng->bw + 1) % 6;
    STATUSLINE_SetText("BW: %d", rng->bw);
    break;
  case 6: // Radio
    rng->radio = (rng->radio + 1) % 3;
    STATUSLINE_SetText("Radio: %s",
                       rng->radio == 0   ? "BK4819"
                       : rng->radio == 1 ? "BK1080"
                                           : "SI4732");
    break;
  case 7: // Squelch type
    rng->squelch.type = (rng->squelch.type + 1) % 4;
    STATUSLINE_SetText("SQL: %s",
                       rng->squelch.type == 0   ? "OFF"
                       : rng->squelch.type == 1 ? "NOI"
                       : rng->squelch.type == 2 ? "N+M"
                                                  : "M");
    break;
  case 8: // Gain
    rng->gainIndex = (rng->gainIndex + 1) % 32;
    STATUSLINE_SetText("Gain: %d", rng->gainIndex);
    break;
  case 9: // TX allow
    rng->allowTx = !rng->allowTx;
    STATUSLINE_SetText("TX: %s", rng->allowTx ? "ON" : "OFF");
    break;
  }
}

static void renderRangeEditMode(void) {
  if (gRangeEditMode == REDIT_MODE_FINPUT) {
    FINPUT_render();
    return;
  }

  RangeEntry *rng = RLST_Item(gEditRangeIndex);
  uint8_t sel = gEditRangeField;

  PrintMediumEx(LCD_XCENTER, 8, POS_C, C_FILL, "Edit Range #%u",
                gEditRangeIndex);
  DrawLine(0, 10, LCD_WIDTH - 1, 10, C_FILL);

#define RFIELD_Y(f) (17 + (f) * 7)
  FillRect(0, RFIELD_Y(sel) - 5, LCD_WIDTH, 7, C_FILL);

  for (uint8_t i = 0; i < REDIT_FIELD_COUNT; i++) {
    uint8_t y = RFIELD_Y(i);
    uint8_t color = (i == sel) ? C_INVERT : C_FILL;

    switch (i) {
    case 0:
      PrintSmallEx(3, y, POS_L, color, "Name: %s", rng->name);
      break;
    case 1:
      PrintSmallEx(3, y, POS_L, color, "Start: %lu.%05lu", rng->start / MHZ,
                   rng->start % MHZ);
      break;
    case 2:
      PrintSmallEx(3, y, POS_L, color, "End: %lu.%05lu", rng->end / MHZ,
                   rng->end % MHZ);
      break;
    case 3:
      PrintSmallEx(3, y, POS_L, color, "Step: %d", rng->step);
      break;
    case 4:
      PrintSmallEx(
          3, y, POS_L, color, "Mod: %s",
          rng->modulation == 0   ? "FM"
          : rng->modulation == 1 ? "AM"
          : rng->modulation == 2 ? "WFM"
          : rng->modulation == 3 ? "USB"
                                   : "LSB");
      break;
    case 5:
      PrintSmallEx(3, y, POS_L, color, "BW: %d", rng->bw);
      break;
    case 6:
      PrintSmallEx(3, y, POS_L, color, "Radio: %s",
                   rng->radio == 0   ? "BK4819"
                   : rng->radio == 1 ? "BK1080"
                                       : "SI4732");
      break;
    case 7:
      PrintSmallEx(3, y, POS_L, color, "SQL: %s:%d",
                   rng->squelch.type == 0   ? "OFF"
                   : rng->squelch.type == 1 ? "NOI"
                   : rng->squelch.type == 2 ? "N+M"
                                              : "M",
                   rng->squelch.value);
      break;
    case 8:
      PrintSmallEx(3, y, POS_L, color, "Gain: %d", rng->gainIndex);
      break;
    case 9:
      PrintSmallEx(3, y, POS_L, color, "TX: %s", rng->allowTx ? "ON" : "OFF");
      break;
    }
  }

  PrintSmallEx(LCD_XCENTER, 63, POS_C, C_FILL, "UP/DN:Field MENU:Edit");
#undef RFIELD_Y
}

static bool rangeEditModeKey(KEY_Code_t key, Key_State_t state) {
  if (gRangeEditMode == REDIT_MODE_FINPUT) {
    return false;
  }

  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      gRangeEditMode = REDIT_MODE_NONE;
      return true;
    case KEY_MENU:
      editRangeField(gEditRangeIndex, gEditRangeField);
      return true;
    case KEY_UP:
      if (gEditRangeField > 0)
        gEditRangeField--;
      return true;
    case KEY_DOWN:
      if (gEditRangeField < REDIT_FIELD_COUNT - 1)
        gEditRangeField++;
      return true;
    default:
      break;
    }
  }

  return false;
}

static Menu rangeMenu = {"Ranges", .render_item = renderItem, .action = action};

static void initMenu() {
  rangeMenu.num_items = RLST_Size();
  rangeMenu.itemHeight = RANGE_MENU_ITEM_H;
  MENU_Init(&rangeMenu);
}

void RANGELIST_render(void) {
  if (gRangeEditMode != REDIT_MODE_NONE) {
    renderRangeEditMode();
    return;
  }

  MENU_Render();
}

void RANGELIST_update() {
  // Nothing to update
}

void RANGELIST_init(void) {
  initMenu();
  if (RLST_Size()) {
    tuneToRange(RLST_Item(menuIndex), false);
  }
}

bool RANGELIST_key(KEY_Code_t key, Key_State_t state) {
  if (gRangeEditMode != REDIT_MODE_NONE) {
    if (rangeEditModeKey(key, state)) {
      return true;
    }
    if (gRangeEditMode == REDIT_MODE_FINPUT) {
      return FINPUT_key(key, state);
    }
    return false;
  }

  if (MENU_HandleInput(key, state)) {
    return true;
  }

  return false;
}
