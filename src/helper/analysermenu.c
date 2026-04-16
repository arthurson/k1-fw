#include "analysermenu.h"
#include "../driver/bk4829.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/menu.h"
#include "../misc.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"

// ---------------------------------------------------------------------------

static bool inMenu;

#define ANALYSERMENU_MAX_ITEMS 10

static MenuItem menuItems[ANALYSERMENU_MAX_ITEMS];
static uint8_t numItems;

static Menu analyserMenu = {
    .title = "Settings",
    .items = menuItems,
    .itemHeight = 7,
    .width = 48,
    .x = LCD_WIDTH - 48,
};

// ---------------------------------------------------------------------------
// dBm range settings (persisted by analyser.c via analyser.set)
// ---------------------------------------------------------------------------

static int16_t dbmMin = -120;
static int16_t dbmMax = -20;
static bool dirty;

int16_t ANALYSERMENU_GetDbmMin(void) { return dbmMin; }
int16_t ANALYSERMENU_GetDbmMax(void) { return dbmMax; }
void ANALYSERMENU_SetDbmMin(int16_t v) { dbmMin = v; }
void ANALYSERMENU_SetDbmMax(int16_t v) { dbmMax = v; }

bool ANALYSERMENU_IsDirty(void) { return dirty; }
void ANALYSERMENU_ClearDirty(void) { dirty = false; }

// ---------------------------------------------------------------------------
// Spur parameters — direct BK4829 register fields
// ---------------------------------------------------------------------------

typedef struct {
  const char *name;
  uint8_t reg;
  uint8_t shift;
  uint8_t width;
  uint16_t maxVal; // 0 = full register
  uint16_t step;   // display step (1=raw, 100=divide by 100)
} SpurParam;

static const SpurParam spurParams[] = {
    // {name, reg, shift, width, maxVal, step}
    {"DSP V", 0x37, 12, 3, 7, 1},    // REG_37<14:12> dsp voltage
    {"vReg",  0x1A, 12, 4, 15, 1},   // REG_1A<15:12> vReg
    {"iBit",  0x1A, 8,  4, 15, 1},   // REG_1A<11:8> iBit
    {"pllCp", 0x1F, 8,  4, 15, 1},   // REG_1F<11:8> pll_cp
    {"vcoLdo",0x1F, 12, 4, 15, 1},   // REG_1F<15:12> vco_ldo_lvl
    {"Bnd3E", 0x3E, 0,  0, 0, 100},  // REG_3E threshold (step 100)
    {"IF_C",  0x1C, 0,  0, 0, 1},    // REG_1C IF filter (full reg)
    {"IF_D",  0x1D, 0,  0, 0, 1},    // REG_1D IF filter (full reg)
};

#define SPUR_COUNT ARRAY_SIZE(spurParams)

static uint16_t readSpurParam(uint8_t idx) {
  const SpurParam *p = &spurParams[idx];
  uint16_t regVal = BK4819_ReadRegister(p->reg);
  if (p->maxVal == 0) return regVal;
  return (regVal >> p->shift) & ((1 << p->width) - 1);
}

static void writeSpurParam(uint8_t idx, uint16_t val) {
  const SpurParam *p = &spurParams[idx];
  uint16_t regVal = BK4819_ReadRegister(p->reg);

  if (p->maxVal == 0) {
    BK4819_WriteRegister(p->reg, val);
    gRedrawScreen = true;
    return;
  }

  uint16_t mask = ((1 << p->width) - 1) << p->shift;
  uint16_t newVal = (regVal & ~mask) | ((val << p->shift) & mask);
  BK4819_WriteRegister(p->reg, newVal);
  gRedrawScreen = true;
}

// ---------------------------------------------------------------------------
// Menu callbacks
// ---------------------------------------------------------------------------

static void getValDbmMin(const MenuItem *item, char *buf, uint8_t buf_size) {
  (void)item;
  sprintf(buf, "%d", dbmMin);
}

static void getValDbmMax(const MenuItem *item, char *buf, uint8_t buf_size) {
  (void)item;
  sprintf(buf, "%d", dbmMax);
}

static void updateValDbmMin(const MenuItem *item, bool up) {
  (void)item;
  dbmMin = up ? dbmMin + 1 : dbmMin - 1;
  if (dbmMin < -140) dbmMin = -140;
  if (dbmMin > 9) dbmMin = 9;
  if (dbmMax <= dbmMin) dbmMax = dbmMin + 1;
  dirty = true;
  gRedrawScreen = true;
}

static void updateValDbmMax(const MenuItem *item, bool up) {
  (void)item;
  dbmMax = up ? dbmMax + 1 : dbmMax - 1;
  if (dbmMax < -139) dbmMax = -139;
  if (dbmMax > 10) dbmMax = 10;
  if (dbmMin >= dbmMax) dbmMin = dbmMax - 1;
  dirty = true;
  gRedrawScreen = true;
}

static void getValSpur(const MenuItem *item, char *buf, uint8_t buf_size) {
  uint8_t idx = (uint8_t)(uintptr_t)item->submenu;
  uint16_t v = readSpurParam(idx);
  uint16_t step = spurParams[idx].step;
  if (step == 1) {
    sprintf(buf, "%u", v);
  } else {
    sprintf(buf, "%u", v / step);
  }
}

static void updateValSpur(const MenuItem *item, bool up) {
  uint8_t idx = (uint8_t)(uintptr_t)item->submenu;
  uint16_t v = readSpurParam(idx);
  uint16_t maxVal = spurParams[idx].maxVal;
  if (maxVal == 0) maxVal = 0xFFFF;
  v = up ? v + spurParams[idx].step : v - spurParams[idx].step;
  if (v > maxVal) v = 0;
  writeSpurParam(idx, v);
}

// ---------------------------------------------------------------------------
// Build menu
// ---------------------------------------------------------------------------

typedef struct {
  const char *name;
  void (*get_value_text)(const MenuItem *, char *, uint8_t);
  void (*change_value)(const MenuItem *, bool);
  uint8_t spurIdx; // 0xFF = db param, 0..SPUR_COUNT-1 = spur param
} AnalyserMenuEntry;

static const AnalyserMenuEntry entries[] = {
    {"Min dB:", getValDbmMin, updateValDbmMin, 0xFF},
    {"Max dB:", getValDbmMax, updateValDbmMax, 0xFF},
    {"DSP V",  getValSpur, updateValSpur, 0},
    {"vReg",   getValSpur, updateValSpur, 1},
    {"iBit",   getValSpur, updateValSpur, 2},
    {"pllCp",  getValSpur, updateValSpur, 3},
    {"vcoLdo", getValSpur, updateValSpur, 4},
    {"Bnd3E",  getValSpur, updateValSpur, 5},
    {"IF_C",   getValSpur, updateValSpur, 6},
    {"IF_D",   getValSpur, updateValSpur, 7},
};

static void renderAnalyserMenuItem(uint16_t index, uint8_t visIndex) {
  const MenuItem *item = &analyserMenu.items[index];
  const uint8_t ex = analyserMenu.x + analyserMenu.width;
  const uint8_t y = analyserMenu.y + visIndex * analyserMenu.itemHeight;
  const uint8_t by = y + analyserMenu.itemHeight - 2;

  char value_buf[16];
  item->get_value_text(item, value_buf, sizeof(value_buf));

  PrintSmall(analyserMenu.x + 2, by, "%s%s", item->name, value_buf);
}

static void initMenu(void) {
  numItems = 0;

  for (uint8_t i = 0; i < ARRAY_SIZE(entries); ++i) {
    MenuItem *m = &menuItems[numItems++];
    m->name = entries[i].name;
    m->setting = 0;
    m->get_value_text = entries[i].get_value_text;
    m->change_value = entries[i].change_value;
    m->submenu = (struct Menu *)(uintptr_t)entries[i].spurIdx;
    m->action = NULL;
  }

  analyserMenu.num_items = numItems;
  analyserMenu.render_item = renderAnalyserMenuItem;
  analyserMenu.y = SPECTRUM_Y;
  analyserMenu.height = numItems * analyserMenu.itemHeight;
  MENU_Init(&analyserMenu);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ANALYSERMENU_Draw(void) {
  if (inMenu) {
    MENU_Render();
  }
}

bool ANALYSERMENU_Key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_EXIT:
      if (inMenu) {
        inMenu = false;
        MENU_Deinit();
        return true;
      }
      break;
    default:
      break;
    }
  }

  if (inMenu && MENU_HandleInput(key, state)) {
    return true;
  }

  return inMenu;
}

// Called from analyser.c on long KEY_0
bool ANALYSERMENU_Toggle(void) {
  inMenu = !inMenu;
  if (inMenu) {
    initMenu();
  } else {
    MENU_Deinit();
  }
  return inMenu;
}

bool ANALYSERMENU_IsActive(void) {
  return inMenu;
}
