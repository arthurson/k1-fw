#ifndef VFOMENU_H
#define VFOMENU_H

#include "../helper/keymap.h"
#include <stdbool.h>

void VFOMENU_Draw(void);
bool VFOMENU_Key(KEY_Code_t key, Key_State_t state);

#endif // VFOMENU_H
