#ifndef KEYMAP_H
#define KEYMAP_H

#include "../driver/keyboard.h"
#include <stdint.h>

// -1 зарезервирован: param не задан (дефолтное поведение действия)
#define KA_PARAM_DEFAULT ((int16_t) - 1)

typedef enum {
  KA_NONE = 0,

  // --- Приложения ---
  KA_APP_LAUNCH, // param = AppType
  KA_EXIT_APP,

  // --- Частота и навигация по каналам ---
  KA_FREQ_INPUT,
  KA_VFO_MODE, // F <-> CH
  KA_NEXT_VFO,
  KA_NEXT_CH,
  KA_PREV_CH,
  KA_TUNE_TO_LOOT,

  // --- Параметры радио (param = шаг: +N вверх, -N вниз) ---
  KA_STEP,
  KA_BW,
  KA_MODULATION,
  KA_SQUELCH,
  KA_GAIN,
  KA_POWER,
  KA_VOLUME,
  KA_AFC,
  KA_DEV,
  KA_FILTER,
  KA_SCRAMBLER,
  KA_RADIO,
  KA_XTAL,
  KA_OFFSET,
  KA_OFFSET_DIR,

  // --- Скан и списки ---
  KA_LOOTLIST,
  KA_CH_LIST,
  KA_MULTIWATCH,
  KA_BLACKLIST_LAST,
  KA_WHITELIST_LAST,
  KA_NEXT_BLACKLIST,
  KA_NEXT_WHITELIST,
  KA_CLEAR_LOOT,
  KA_SAVE_LOOT_CH,

  // --- TX / PTT ---
  KA_TX,
  KA_PTT,
  KA_VOX,
  KA_MONI,

  // --- Отображение ---
  KA_RSSI,
  KA_RSSI_GRAPH,
  KA_ALWAYS_RSSI,
  KA_LEVEL_DISPLAY,
  KA_GRAPH_UNIT,
  KA_VFO_MENU,
  KA_RADIO_SETTINGS,
  KA_PRO_MODE,

  // --- Полоса / диапазон (специфично для сканера) ---
  KA_BANDS,
  KA_CHANNELS,
  KA_BAND_UP,
  KA_BAND_DOWN,
  KA_ZOOM_IN,
  KA_ZOOM_OUT,
  KA_RANGE_INPUT,

  // --- Быстрые настройки ---
  KA_BL,
  KA_BL_MAX,
  KA_BL_MIN,
  KA_CONTRAST,
  KA_BEEP,
  KA_INVERT_BTNS,
  KA_FLASHLIGHT,

  // --- Прочее ---
  KA_FASTMENU1,
  KA_FASTMENU2,

  KA_COUNT,
} KeyAction;

typedef struct {
  KeyAction action;
  int16_t param; // KA_PARAM_DEFAULT = не задан
} AppAction_t;

typedef struct {
  AppAction_t click[KEY_COUNT];      // KEY_RELEASED
  AppAction_t long_press[KEY_COUNT]; // KEY_LONG_PRESSED
} AppKeymap_t;

void KEYMAP_Load(void);
void KEYMAP_Save(void);

extern AppKeymap_t gCurrentKeymap;
extern const char *KA_NAMES[];

#endif // KEYMAP_H
