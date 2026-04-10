#include "sqviewer.h"
#include "../helper/measurements.h"
#include "../helper/storage.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "apps.h"
#include <string.h>

static SquelchPreset presets[SQ_PRESETS_COUNT];
static uint8_t selectedLevel = 0;
static char currentFile[64] = {0};
static bool editing = false;
static uint32_t editingValue;  // Temp value being edited

void SQVIEWER_init(void) {
  // Get filename from global
  if (gOpenedFile[0] != '\0') {
    strncpy(currentFile, gOpenedFile, sizeof(currentFile) - 1);
    currentFile[sizeof(currentFile) - 1] = '\0';
  } else {
    // Default to VHF
    strcpy(currentFile, "/vhf.sq");
  }

  // Load presets from file
  memset(presets, 0, sizeof(presets));
  for (uint8_t i = 0; i < SQ_PRESETS_COUNT; i++) {
    Storage_Load(currentFile, i, &presets[i], sizeof(SquelchPreset));
  }

  selectedLevel = 0;
  editing = false;
  gRedrawScreen = true;
}

void SQVIEWER_deinit(void) {}

bool SQVIEWER_key(KEY_Code_t key, Key_State_t state) {
  if (state != KEY_RELEASED) return false;

  switch (key) {
  case KEY_UP:
    if (selectedLevel > 0) selectedLevel--;
    else selectedLevel = SQ_PRESETS_COUNT - 1;
    editing = false;
    gRedrawScreen = true;
    return true;

  case KEY_DOWN:
    if (selectedLevel < SQ_PRESETS_COUNT - 1) selectedLevel++;
    else selectedLevel = 0;
    editing = false;
    gRedrawScreen = true;
    return true;

  case KEY_MENU:
    editing = !editing;
    if (editing) {
      editingValue = presets[selectedLevel].ro;
    } else {
      // Save changes
      Storage_Save(currentFile, selectedLevel, &presets[selectedLevel],
                   sizeof(SquelchPreset));
    }
    gRedrawScreen = true;
    return true;

  case KEY_1:
  case KEY_7:
    if (editing) {
      int32_t delta = (key == KEY_1) ? 1 : -1;
      presets[selectedLevel].ro =
          AdjustU(presets[selectedLevel].ro, 0, 255, delta);
      presets[selectedLevel].rc =
          presets[selectedLevel].ro > 4 ? presets[selectedLevel].ro - 4 : 0;
      gRedrawScreen = true;
    }
    return true;

  case KEY_2:
  case KEY_8:
    if (editing) {
      int32_t delta = (key == KEY_2) ? 1 : -1;
      presets[selectedLevel].no =
          AdjustU(presets[selectedLevel].no, 0, 128, delta);
      presets[selectedLevel].nc = presets[selectedLevel].no + 4;
      gRedrawScreen = true;
    }
    return true;

  case KEY_3:
  case KEY_9:
    if (editing) {
      int32_t delta = (key == KEY_3) ? 1 : -1;
      presets[selectedLevel].go =
          AdjustU(presets[selectedLevel].go, 0, 255, delta);
      presets[selectedLevel].gc = presets[selectedLevel].go + 4;
      gRedrawScreen = true;
    }
    return true;

  case KEY_EXIT:
    if (editing) {
      editing = false;
      gRedrawScreen = true;
      return true;
    }
    APPS_exit();
    return true;

  default:
    break;
  }
  return false;
}

void SQVIEWER_render(void) {
  STATUSLINE_RenderRadioSettings();

  // File name at top
  const char *shortName = strrchr(currentFile, '/');
  shortName = shortName ? shortName + 1 : currentFile;
  PrintSmallEx(LCD_XCENTER, 0, POS_C, C_FILL, "%s", shortName);

  // Preset list
  for (uint8_t i = 0; i < SQ_PRESETS_COUNT; i++) {
    uint8_t y = 8 + i * 5;
    SquelchPreset *p = &presets[i];

    // Highlight selected
    if (i == selectedLevel) {
      if (editing) {
        // Editing mode: highlight with inverse
        FillRect(0, y, LCD_WIDTH, 5, C_FILL);
        PrintSmallEx(LCD_WIDTH - 1, y + 1, POS_R, C_CLEAR, "%u R%u N%u G%u", i,
                     p->ro, p->no, p->go);
      } else {
        // Selected but not editing
        PrintSmallEx(LCD_WIDTH - 1, y + 1, POS_R, C_FILL, "%u R%u N%u G%u <", i,
                     p->ro, p->no, p->go);
      }
    } else {
      PrintSmallEx(LCD_WIDTH - 1, y + 1, POS_R, C_FILL, "  %u R%u N%u G%u", i,
                   p->ro, p->no, p->go);
    }
  }

  // Help text at bottom
  if (!editing) {
    PrintSmallEx(LCD_XCENTER, LCD_HEIGHT - 7, POS_C, C_FILL, "MENU edit");
  } else {
    PrintSmallEx(LCD_XCENTER, LCD_HEIGHT - 7, POS_C, C_FILL,
                 "1/7R 2/8N 3/9G MENU save");
  }
}
