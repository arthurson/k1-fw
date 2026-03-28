#include "board.h"
#include "driver/audio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "helper/audio_io.h"
#include "helper/pocsag.h"
#include "system.h"
#include "ui/graphics.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
  SYSTICK_Init();

  BOARD_Init();
  GPIO_TurnOnBacklight();

  AUDIO_IO_Init();

  SYS_Main();
}
