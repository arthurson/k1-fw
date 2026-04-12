#ifndef ANALYSERMENU_H
#define ANALYSERMENU_H

#include "../helper/keymap.h"
#include <stdbool.h>
#include <stdint.h>

void ANALYSERMENU_Draw(void);
bool ANALYSERMENU_Key(KEY_Code_t key, Key_State_t state);

// Open/close the menu, returns true if menu is now open
bool ANALYSERMENU_Toggle(void);
bool ANALYSERMENU_IsActive(void);

// Dirty flag for debounced saving
bool ANALYSERMENU_IsDirty(void);
void ANALYSERMENU_ClearDirty(void);

// Get current dBm values from the menu settings
int16_t ANALYSERMENU_GetDbmMin(void);
int16_t ANALYSERMENU_GetDbmMax(void);
void ANALYSERMENU_SetDbmMin(int16_t dbmMin);
void ANALYSERMENU_SetDbmMax(int16_t dbmMax);

#endif // ANALYSERMENU_H
