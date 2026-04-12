#include "board.h"
#include "driver/audio.h"
#include "driver/backlight.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/hrtime.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "helper/audio_io.h"
#include "helper/pocsag.h"
#include "system.h"
#include "ui/graphics.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

void testScan() {
  BK4819_Init();
  BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
  BK4819_RX_TurnOn();

  BK4819_SetAFC(0);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);

  BK4819_SelectFilterEx(FILTER_UHF);

  uint32_t stp = 25 * KHZ;
  uint32_t s = 433 * MHZ;
  uint32_t e = s + stp * LCD_WIDTH;

  uint16_t v[LCD_WIDTH];
  uint16_t peakRssi;
  uint8_t peakX;

  static const int8_t kGrid[] = {-60, -80, -100};

  for (;;) {
    UI_ClearScreen();

    // сканирование + поиск пика
    peakRssi = 0;
    peakX = 0;
    uint8_t i = 0;
    for (uint32_t f = s; f < e; f += stp) {
      BK4819_TuneTo(f, false);
      SYSTICK_DelayUs(2200);
      v[i] = BK4819_GetRSSI();
      if (v[i] > peakRssi) {
        peakRssi = v[i];
        peakX = i;
      }
      i++;
    }

    // бары
    for (uint8_t x = 0; x < LCD_WIDTH; x++) {
      int16_t dBm = Rssi2DBm(v[x]);
      uint32_t y = ConvertDomain(dBm, -120, -50, LCD_HEIGHT, 8);
      DrawVLine(x, y, LCD_HEIGHT - y, C_FILL);
    }

    // сетка: дашированные линии + подпись слева
    for (uint8_t g = 0; g < ARRAY_SIZE(kGrid); g++) {
      int16_t gy = ConvertDomain(kGrid[g], -120, -50, LCD_HEIGHT, 8);
      for (int16_t gx = 20; gx < LCD_WIDTH; gx += 4)
        DrawHLine(gx, gy, 2, C_INVERT);
      PrintSmall(0, gy, "%d", kGrid[g]); // baseline совпадает с линией
    }

    // пик: уровень и частота
    int16_t peakDBm = Rssi2DBm(peakRssi);
    uint32_t peakFreq = s + (uint32_t)peakX * stp;
    PrintSmallEx(LCD_WIDTH - 1, 14, POS_R, C_FILL, "%ddBm %u.%03uM", peakDBm,
                 peakFreq / MHZ, (peakFreq % MHZ) / KHZ);

    ST7565_Blit();
  }
}

int main(void) {
  SYSTICK_Init();
  HRTIME_Init();

  BOARD_Init();
  GPIO_TurnOnBacklight();
  BACKLIGHT_SetBrightness(2);

  // AUDIO_IO_Init(); // Отключено — ADC/DAC DMA создают RF помехи

  SYS_Main();
}
