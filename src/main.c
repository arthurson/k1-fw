#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "helper/audio_io.h"
#include "system.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
  SYSTICK_Init();

  BOARD_Init();

  AUDIO_IO_Init();

  GPIO_TurnOnBacklight();

  SYS_Main();
}
