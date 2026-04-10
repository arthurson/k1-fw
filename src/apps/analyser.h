#ifndef ANALYSER_H
#define ANALYSER_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

bool ANALYSER_key(KEY_Code_t Key, Key_State_t state);
void ANALYSER_init(void);
void ANALYSER_deinit(void);
void ANALYSER_update(void);
void ANALYSER_render(void);

#endif /* end of include guard: ANALYSER_H */
