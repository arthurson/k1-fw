#include "board.h"
#include "driver/audio.h"
#include "driver/backlight.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/hrtime.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "external/CMSIS/Device/PY32F071/Include/py32f071xB.h"
#include "helper/pocsag.h"
#include "system.h"
#include "ui/graphics.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#define PIN_CSN GPIO_MAKE_PIN(GPIOF, LL_GPIO_PIN_9)
#define PIN_SCL GPIO_MAKE_PIN(GPIOB, LL_GPIO_PIN_8)
#define PIN_SDA GPIO_MAKE_PIN(GPIOB, LL_GPIO_PIN_9)
#if !defined(HSE_VALUE)
#define HSE_VALUE 24000000U /*!< Value of the External oscillator in Hz */
#endif                      /* HSE_VALUE */

#if !defined(HSI_VALUE)
#define HSI_VALUE 8000000U /*!< Value of the Internal oscillator in Hz*/
#endif                     /* HSI_VALUE */

#if !defined(LSI_VALUE)
#define LSI_VALUE 32768U /*!< Value of LSI in Hz*/
#endif                   /* LSI_VALUE */

#if !defined(LSE_VALUE)
#define LSE_VALUE 32768U /*!< Value of LSE in Hz*/
#endif                   /* LSE_VALUE */
void PrintClockConfiguration(void) {
  // Получаем текущую информацию о тактировании
  uint32_t sysclk_source = RCC->CFGR & RCC_CFGR_SWS;
  uint32_t pll_source = RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC;
  uint32_t pll_mul =
      ((RCC->PLLCFGR & RCC_PLLCFGR_PLLMUL) >> RCC_PLLCFGR_PLLMUL_Pos);

  // Определяем источник системной частоты
  printf("System clock source: ");
  switch (sysclk_source) {
  case 0x00:
    printf("HSI\n");
    break;
  case 0x01:
    printf("HSE\n");
    break;
  case 0x02:
    printf("PLL\n");
    break;
  case 0x03:
    printf("LSI\n");
    break;
  default:
    printf("Unknown\n");
  }

  // Если используется PLL, показываем его параметры
  if (sysclk_source == 0x02) {
    const uint8_t pllMulValue[4] = {2, 3, 2, 2};
    uint32_t actual_mul = pllMulValue[pll_mul];

    printf("PLL multiplier: %d\n", actual_mul);

    if (pll_source == RCC_PLLCFGR_PLLSRC_HSI) {
      uint32_t hsifs = (RCC->ICSCR & RCC_ICSCR_HSI_FS) >> RCC_ICSCR_HSI_FS_Pos;
      uint32_t hsi_freq = HSIFreqTable[hsifs];
      printf("PLL source: HSI (%d Hz)\n", hsi_freq);
      printf("Calculated PLL frequency: %d Hz\n", hsi_freq * actual_mul);
    } else {
      printf("PLL source: HSE (%d Hz)\n", HSE_VALUE);
      printf("Calculated PLL frequency: %d Hz\n", HSE_VALUE * actual_mul);
    }
  }

  // Показываем текущую частоту ядра
  printf("SystemCoreClock: %d Hz\n", SystemCoreClock);

  // Проверяем настройки GPIO для SPI
  printf("SCL GPIO speed: %d\n",
         LL_GPIO_GetPinSpeed(GPIO_PORT(PIN_SCL), GPIO_PIN_MASK(PIN_SCL)));
  printf("SDA GPIO speed: %d\n",
         LL_GPIO_GetPinSpeed(GPIO_PORT(PIN_SDA), GPIO_PIN_MASK(PIN_SDA)));
}

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
  PrintClockConfiguration();

  SYS_Main();
}
