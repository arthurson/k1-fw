#include "analysermenu.h"
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

#define ANALYSERMENU_MAX_ITEMS 2

static MenuItem menuItems[ANALYSERMENU_MAX_ITEMS];
static uint8_t numItems;

static Menu analyserMenu = {
    .title = "Spectrum Range",
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

// ---------------------------------------------------------------------------
// Build menu
// ---------------------------------------------------------------------------

typedef struct {
  const char *name;
  void (*get_value_text)(const MenuItem *, char *, uint8_t);
  void (*change_value)(const MenuItem *, bool);
} AnalyserMenuEntry;

static const AnalyserMenuEntry entries[] = {
    {"Min dB:", getValDbmMin, updateValDbmMin},
    {"Max dB:", getValDbmMax, updateValDbmMax},
};

static void renderAnalyserMenuItem(uint16_t index, uint8_t visIndex) {
  const MenuItem *item = &analyserMenu.items[index];
  const uint8_t ex = analyserMenu.x + analyserMenu.width;
  const uint8_t y = analyserMenu.y + visIndex * analyserMenu.itemHeight;
  const uint8_t by = y + analyserMenu.itemHeight - 2;

  // Render name+value as one string inside the menu rect
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
    m->submenu = NULL;
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
