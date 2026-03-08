#pragma once

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern bool gHasUnreadMessages;

void MESSENGER_init(void);
void MESSENGER_deinit(void);
void MESSENGER_update(void);
bool MESSENGER_key(KEY_Code_t key, Key_State_t state);
void MESSENGER_render(void);
