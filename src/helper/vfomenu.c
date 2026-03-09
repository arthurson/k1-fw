#include "vfomenu.h"
#include "../apps/apps.h"
#include "../dcs.h"
#include "../driver/systick.h"
#include "../external/printf/printf.h"
#include "../helper/menu.h"
#include "../radio.h"
#include "../ui/finput.h"

// ---------------------------------------------------------------------------
// Виртуальные ID для параметров без ParamType (code type)
// ---------------------------------------------------------------------------

#define VP_RX_CODE_TYPE ((ParamType)(PARAM_COUNT + 0))
#define VP_TX_CODE_TYPE ((ParamType)(PARAM_COUNT + 1))

// ---------------------------------------------------------------------------

static bool inMenu;

#define VFOMENU_MAX_ITEMS 9

static MenuItem menuItems[VFOMENU_MAX_ITEMS];
static uint8_t numItems;

static Menu vfoMenu = {
    .title = "VFO Params",
    .items = menuItems,
    .itemHeight = 7,
    .width = 80,
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static VFOContext *getCtx(void) {
  return &RADIO_GetCurrentVFO(gRadioState)->context;
}

static const char *codeTypeNames[] = {
    [CODE_TYPE_OFF] = "None",
    [CODE_TYPE_CONTINUOUS_TONE] = "CTCSS",
    [CODE_TYPE_DIGITAL] = "DCS",
    [CODE_TYPE_REVERSE_DIGITAL] = "RDCS",
};

#define CODE_TYPE_COUNT 4

// ---------------------------------------------------------------------------
// get_value_text callbacks
// ---------------------------------------------------------------------------

static void getValGeneric(const MenuItem *item, char *buf, uint8_t buf_size) {
  VFOContext *c = getCtx();
  sprintf(buf, "%s", RADIO_GetParamValueString(c, item->setting));
}

static void getValRxCodeType(const MenuItem *item, char *buf,
                             uint8_t buf_size) {
  (void)item;
  VFOContext *c = getCtx();
  sprintf(buf, "%s", codeTypeNames[c->code.type]);
}

static void getValTxCodeType(const MenuItem *item, char *buf,
                             uint8_t buf_size) {
  (void)item;
  VFOContext *c = getCtx();
  sprintf(buf, "%s", codeTypeNames[c->tx_state.code.type]);
}

// ---------------------------------------------------------------------------
// change_value callbacks
// ---------------------------------------------------------------------------

static void updateValGeneric(const MenuItem *item, bool up) {
  VFOContext *c = getCtx();
  RADIO_IncDecParam(c, item->setting, up, true);
}

static void updateValRxCodeType(const MenuItem *item, bool up) {
  (void)item;
  VFOContext *c = getCtx();
  uint8_t t = c->code.type;
  t = up ? (t + 1) % CODE_TYPE_COUNT
         : (t + CODE_TYPE_COUNT - 1) % CODE_TYPE_COUNT;
  c->code.type = t;
  // Сброс значения при смене типа
  c->code.value = 0;
  c->dirty[PARAM_RX_CODE] = true;
  c->save_to_eeprom = true;
  c->last_save_time = Now();
  RADIO_ApplySettings(c);
}

static void updateValTxCodeType(const MenuItem *item, bool up) {
  (void)item;
  VFOContext *c = getCtx();
  uint8_t t = c->tx_state.code.type;
  t = up ? (t + 1) % CODE_TYPE_COUNT
         : (t + CODE_TYPE_COUNT - 1) % CODE_TYPE_COUNT;
  c->tx_state.code.type = t;
  c->tx_state.code.value = 0;
  c->dirty[PARAM_TX_CODE] = true;
  c->save_to_eeprom = true;
  c->last_save_time = Now();
  RADIO_ApplySettings(c);
}
static void setOffset(uint32_t v, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_TX_OFFSET, v, true);
}
static bool offsetAction(const MenuItem *item, KEY_Code_t key,
                         Key_State_t state) {
  (void)item;

  if (state != KEY_RELEASED || key != KEY_MENU) {
    return false;
  }

  FINPUT_setup(0, 50000000, UNIT_MHZ, false);
  FINPUT_Show(setOffset);
  return true;
}

// ---------------------------------------------------------------------------
// Сборка меню
// ---------------------------------------------------------------------------

typedef struct {
  const char *name;
  ParamType param; // VP_* или реальный ParamType
  void (*get_value_text)(const MenuItem *, char *, uint8_t);
  void (*change_value)(const MenuItem *, bool);
  bool (*action)(const MenuItem *item, KEY_Code_t, Key_State_t);
} VfoMenuEntry;

static const VfoMenuEntry entries[] = {
    {"RX type", VP_RX_CODE_TYPE, getValRxCodeType, updateValRxCodeType},
    {"RX code", PARAM_RX_CODE, getValGeneric, updateValGeneric},
    {"TX type", VP_TX_CODE_TYPE, getValTxCodeType, updateValTxCodeType},
    {"TX code", PARAM_TX_CODE, getValGeneric, updateValGeneric},
    {"Offset", PARAM_TX_OFFSET, getValGeneric, NULL, .action = offsetAction},
    {"Offset dir", PARAM_TX_OFFSET_DIR, getValGeneric, updateValGeneric},
};

static void initMenu(void) {
  numItems = 0;

  for (uint8_t i = 0; i < ARRAY_SIZE(entries); ++i) {
    MenuItem *m = &menuItems[numItems++];
    m->name = entries[i].name;
    m->setting = entries[i].param;
    m->get_value_text = entries[i].get_value_text;
    m->change_value = entries[i].change_value;
    m->action = entries[i].action;
  }

  vfoMenu.num_items = numItems;
  MENU_Init(&vfoMenu);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void VFOMENU_Draw(void) {
  if (inMenu) {
    MENU_Render();
  }
}

// Открыть/закрыть по KEY_HASH (или любому другому — решает caller).
// Возвращает true если событие поглощено.
bool VFOMENU_Key(KEY_Code_t key, Key_State_t state) {
  if (state == KEY_RELEASED) {
    switch (key) {
    case KEY_F:
      if (inMenu) {
        break; // handle menu
      }
      inMenu = !inMenu;
      if (inMenu) {
        initMenu();
      } else {
        MENU_Deinit();
      }
      return true;

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

  return inMenu; // Блокируем события пока меню открыто
}
