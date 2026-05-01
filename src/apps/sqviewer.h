#ifndef SQVIEWER_H
#define SQVIEWER_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

bool SQVIEWER_key(KEY_Code_t Key, Key_State_t state);
void SQVIEWER_init(void);
void SQVIEWER_deinit(void);
void SQVIEWER_render(void);

#endif /* end of include guard: SQVIEWER_H */
