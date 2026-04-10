#ifndef CMDEDIT_H
#define CMDEDIT_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

extern char gCmdEditFilename[32];
void CMDEDIT_SetFilename(const char *filename);
void CMDEDIT_init();
bool CMDEDIT_key(KEY_Code_t key, Key_State_t state);
void CMDEDIT_render(void);

#endif
