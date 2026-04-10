#ifndef RANGELIST_APP_H
#define RANGELIST_APP_H

#include "../driver/keyboard.h"
#include "../helper/rangelist.h"
#include <stdbool.h>
#include <stdint.h>

extern bool gRangelistActive;

void RANGELIST_init();
void RANGELIST_update();
bool RANGELIST_key(KEY_Code_t key, Key_State_t state);
void RANGELIST_render();

#endif
