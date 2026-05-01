#include "sqviewer.h"
#include "../helper/measurements.h"
#include "../helper/menu.h"
#include "../helper/storage.h"
#include "../ui/graphics.h"
#include "apps.h"
#include <string.h>

static SquelchPreset presets[SQ_PRESETS_COUNT];
static char currentFile[64] = {0};

static void renderItem(uint16_t index, uint8_t i) {
  SquelchPreset *p = &presets[index];
  uint8_t y = MENU_Y + i * MENU_ITEM_H;
  // номер пресета
  PrintMediumEx(13, y + 8, POS_L, C_INVERT, "SQ %2u", index);
  // значения справа
  PrintSmallEx(LCD_WIDTH - 5, y + 8, POS_R, C_INVERT, "R:%3u N:%3u G:%3u",
               p->ro, p->no, p->go);
}

static bool action(const uint16_t index, KEY_Code_t key, Key_State_t state) {
  if (state != KEY_RELEASED)
    return false;

  SquelchPreset *p = &presets[index];

  switch (key) {
  case KEY_1:
    p->ro = AdjustU(p->ro, 0, 512, 1);
    p->rc = p->ro > 4 ? p->ro - 4 : 0;
    return true;
  case KEY_7:
    p->ro = AdjustU(p->ro, 0, 512, -1);
    p->rc = p->ro > 4 ? p->ro - 4 : 0;
    return true;
  case KEY_2:
    p->no = AdjustU(p->no, 0, 255, 1);
    p->nc = p->no + 4;
    return true;
  case KEY_8:
    p->no = AdjustU(p->no, 0, 255, -1);
    p->nc = p->no + 4;
    return true;
  case KEY_3:
    p->go = AdjustU(p->go, 0, 255, 1);
    p->gc = p->go + 4;
    return true;
  case KEY_9:
    p->go = AdjustU(p->go, 0, 255, -1);
    p->gc = p->go + 4;
    return true;
  case KEY_MENU:
    Storage_Save(currentFile, index, p, sizeof(SquelchPreset));
    return true;
  default:
    return false;
  }
}

static Menu sqMenu = {
    .render_item = renderItem,
    .itemHeight = MENU_ITEM_H,
    .action = action,
    .num_items = SQ_PRESETS_COUNT,
};

void SQVIEWER_init(void) {
  if (gOpenedFile[0] != '\0') {
    strncpy(currentFile, gOpenedFile, sizeof(currentFile) - 1);
  } else {
    strcpy(currentFile, "/vhf.sq");
  }

  for (uint8_t i = 0; i < SQ_PRESETS_COUNT; i++) {
    Storage_Load(currentFile, i, &presets[i], sizeof(SquelchPreset));
  }

  const char *title = strrchr(currentFile, '/');
  sqMenu.title = title ? title + 1 : currentFile;

  MENU_Init(&sqMenu);
}

void SQVIEWER_deinit(void) { MENU_Deinit(); }

bool SQVIEWER_key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED && key == KEY_EXIT) {
    APPS_exit();
    return true;
  }
  return MENU_HandleInput(key, state);
}

void SQVIEWER_render(void) { MENU_Render(); }
