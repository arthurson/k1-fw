#include "board.h"
#include "driver/backlight.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4829.h"
#include "driver/gpio.h"
#include "driver/lfs.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_adc.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_bus.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_dac.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_dma.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_gpio.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_rcc.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_system.h"
#include "external/PY32F071_HAL_Driver/Inc/py32f071_ll_tim.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include <stdint.h>

// DMA buffer: CH9 only (APRS audio).
// Layout: [half-A: APRS_BUFFER_SIZE samples | half-B: APRS_BUFFER_SIZE samples]
// HT fires when half-A is full; TC fires when half-B is full.
volatile uint16_t adc_dma_buffer[2 * APRS_BUFFER_SIZE]
    __attribute__((aligned(4)));

volatile bool aprs_ready1 = false;
volatile bool aprs_ready2 = false;

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------

void BOARD_DMA_Init(void) {
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

  LL_AHB1_GRP1_ForceReset(LL_AHB1_GRP1_PERIPH_DMA1);
  LL_AHB1_GRP1_ReleaseReset(LL_AHB1_GRP1_PERIPH_DMA1);

  LL_DMA_DeInit(DMA1, LL_DMA_CHANNEL_1);

  LL_DMA_InitTypeDef DMA_InitStruct;
  LL_DMA_StructInit(&DMA_InitStruct);

  DMA_InitStruct.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
  DMA_InitStruct.PeriphOrM2MSrcAddress = (uint32_t)&ADC1->DR;
  DMA_InitStruct.MemoryOrM2MDstAddress = (uint32_t)adc_dma_buffer;
  DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_HALFWORD;
  DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_HALFWORD;
  DMA_InitStruct.NbData =
      2 * APRS_BUFFER_SIZE; // single channel, two ping-pong halves
  DMA_InitStruct.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
  DMA_InitStruct.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
  DMA_InitStruct.Mode = LL_DMA_MODE_CIRCULAR;
  DMA_InitStruct.Priority = LL_DMA_PRIORITY_HIGH;

  LL_DMA_Init(DMA1, LL_DMA_CHANNEL_1, &DMA_InitStruct);

  LL_DMA_EnableIT_TC(DMA1, LL_DMA_CHANNEL_1);
  LL_DMA_EnableIT_HT(DMA1, LL_DMA_CHANNEL_1);
  LL_DMA_EnableIT_TE(DMA1, LL_DMA_CHANNEL_1);
  NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

void DMA1_Channel1_IRQHandler(void) {
  if (LL_DMA_IsActiveFlag_HT1(DMA1)) {
    LL_DMA_ClearFlag_HT1(DMA1);
    // half-A (indices 0..APRS_BUFFER_SIZE-1) стабильна — DMA пишет в half-B
    aprs_ready1 = true;
  }

  if (LL_DMA_IsActiveFlag_TC1(DMA1)) {
    LL_DMA_ClearFlag_TC1(DMA1);
    // half-B (indices APRS_BUFFER_SIZE..2*APRS_BUFFER_SIZE-1) стабильна — DMA
    // пишет в half-A
    aprs_ready2 = true;
  }

  if (LL_DMA_IsActiveFlag_TE1(DMA1)) {
    LL_DMA_ClearFlag_TE1(DMA1);
    LogC(LOG_C_RED, "DMA Transfer Error!");
  }
}

// ---------------------------------------------------------------------------
// GPIO
// ---------------------------------------------------------------------------

void BOARD_GPIO_Init(void) {
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA | LL_IOP_GRP1_PERIPH_GPIOB |
                          LL_IOP_GRP1_PERIPH_GPIOC | LL_IOP_GRP1_PERIPH_GPIOF);

  LL_GPIO_InitTypeDef InitStruct;
  LL_GPIO_StructInit(&InitStruct);
  InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  InitStruct.Pull = LL_GPIO_PULL_UP;

  // --- Input pins ---
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;  // Для входов скорость не важна
  InitStruct.Mode = LL_GPIO_MODE_INPUT;

  // Keypad rows: PB12–PB15
  InitStruct.Pin =
      LL_GPIO_PIN_15 | LL_GPIO_PIN_14 | LL_GPIO_PIN_13 | LL_GPIO_PIN_12;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // PTT: PB10
  InitStruct.Pin = LL_GPIO_PIN_10;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // --- Output pins: LOW speed (медленное переключение) ---
  InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;

  // Keypad cols: PB3–PB6
  InitStruct.Pin =
      LL_GPIO_PIN_6 | LL_GPIO_PIN_5 | LL_GPIO_PIN_4 | LL_GPIO_PIN_3;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // Audio PA: PA8 | Flashlight: PC13 | Backlight: PF8
  InitStruct.Pin = LL_GPIO_PIN_8;
  LL_GPIO_Init(GPIOA, &InitStruct);
  InitStruct.Pin = LL_GPIO_PIN_13;
  LL_GPIO_Init(GPIOC, &InitStruct);
  InitStruct.Pin = LL_GPIO_PIN_8;
  LL_GPIO_Init(GPIOF, &InitStruct);

  // BK1080 I2C: PF5, PF6
  InitStruct.Pin = LL_GPIO_PIN_6 | LL_GPIO_PIN_5;
  LL_GPIO_Init(GPIOF, &InitStruct);

  // --- LCD SPI pins — VERY_HIGH (быстрое переключение в SPI) ---
  InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;

  LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_6); // LCD A0
  InitStruct.Pin = LL_GPIO_PIN_6;
  LL_GPIO_Init(GPIOA, &InitStruct);

  LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_2); // LCD CS
  InitStruct.Pin = LL_GPIO_PIN_2;
  LL_GPIO_Init(GPIOB, &InitStruct);

  // SPI flash CS: PA3
  InitStruct.Pin = LL_GPIO_PIN_3;
  LL_GPIO_Init(GPIOA, &InitStruct);

  // --- BK4829 pins — VERY_HIGH (bit-bang SPI) ---
  InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_8;
  LL_GPIO_Init(GPIOB, &InitStruct);

  InitStruct.Pin = LL_GPIO_PIN_9;
  LL_GPIO_Init(GPIOF, &InitStruct);

  // ADC inputs (analog): PB0, PB1
  InitStruct.Mode = LL_GPIO_MODE_ANALOG;
  InitStruct.Pull = LL_GPIO_PULL_NO;
  InitStruct.Pin = LL_GPIO_PIN_0 | LL_GPIO_PIN_1;
  LL_GPIO_Init(GPIOB, &InitStruct);
}

void BOARD_TIM3_Init(void) {
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);

  LL_TIM_SetPrescaler(TIM3, 0);
  LL_TIM_SetAutoReload(TIM3, 4999); // 48MHz / 5000 = 9600 Hz точно

  LL_TIM_SetCounterMode(TIM3, LL_TIM_COUNTERMODE_UP);
  LL_TIM_SetTriggerOutput(TIM3, LL_TIM_TRGO_UPDATE);
  LL_TIM_DisableMasterSlaveMode(TIM3);
}

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------

void BOARD_ADC_Init(void) {
  LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);

  LL_APB1_GRP2_ForceReset(LL_APB1_GRP2_PERIPH_ADC1);
  LL_APB1_GRP2_ReleaseReset(LL_APB1_GRP2_PERIPH_ADC1);

  // DMA must be initialized before ADC is enabled
  BOARD_DMA_Init();

  LL_ADC_SetCommonPathInternalCh(ADC1_COMMON, LL_ADC_PATH_INTERNAL_NONE);
  LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
  LL_ADC_SetDataAlignment(ADC1, LL_ADC_DATA_ALIGN_RIGHT);

  // -----------------------------------------------------------------------
  // Regular group: CH9 (APRS audio) → DMA circular
  // -----------------------------------------------------------------------
  LL_ADC_SetSequencersScanMode(ADC1, LL_ADC_SEQ_SCAN_ENABLE);

  /* LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE);
LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_CONTINUOUS); */

  LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_EXT_TIM3_TRGO);
  // LL_ADC_REG_SetTriggerEdge(ADC1, LL_ADC_REG_TRIG_EXT_RISING);
  SET_BIT(ADC1->CR2, ADC_CR2_EXTTRIG);
  LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_SINGLE);

  LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
  // Single rank — only CH9
  LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_DISABLE);
  LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_9);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_9,
                                LL_ADC_SAMPLINGTIME_239CYCLES_5);
  /* LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_9,
                                LL_ADC_SAMPLINGTIME_28CYCLES_5); */

  // -----------------------------------------------------------------------
  // Injected group: CH8 (battery voltage) → software-triggered, single shot
  //
  // Per reference manual 16.3.12.1: JAUTO=0, SCAN=1.
  // Injected trigger fires on JSWSTART; result lands in JDR1.
  // -----------------------------------------------------------------------
  LL_ADC_INJ_SetTriggerSource(ADC1, LL_ADC_INJ_TRIG_SOFTWARE);
  LL_ADC_INJ_SetSequencerLength(ADC1, LL_ADC_INJ_SEQ_SCAN_DISABLE); // 1 rank
  LL_ADC_INJ_SetSequencerRanks(ADC1, LL_ADC_INJ_RANK_1, LL_ADC_CHANNEL_8);
  LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_8,
                                LL_ADC_SAMPLINGTIME_5CYCLES_5);
  // Automatic injection disabled (we trigger manually)
  LL_ADC_INJ_SetTrigAuto(ADC1, LL_ADC_INJ_TRIG_INDEPENDENT);

  LL_RCC_SetADCClockSource(LL_RCC_ADC_CLKSOURCE_PCLK_DIV2);

  LL_ADC_StartCalibration(ADC1);
  while (LL_ADC_IsCalibrationOnGoing(ADC1))
    ;

  // Route ADC1 requests to DMA1 Channel 1 via SYSCFG remap
  // (SYSCFG clock is already enabled by UART_Init before this call)
  LL_SYSCFG_SetDMARemap(DMA1, LL_DMA_CHANNEL_1, LL_SYSCFG_DMA_MAP_ADC1);

  // DMA must be enabled before ADC
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);

  LL_ADC_Enable(ADC1);

  LL_TIM_EnableCounter(TIM3);
}

void BOARD_ADC_StartAPRS_DMA(void) {
  if (!LL_ADC_IsEnabled(ADC1)) {
    LL_ADC_Enable(ADC1);
    SYSTICK_DelayUs(10);
  }
  if (!LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_1)) {
    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
  }
  LL_ADC_REG_StartConversionSWStart(ADC1);
}

void BOARD_ADC_StopAPRS_DMA(void) {
  LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
  LL_ADC_StopConversion(ADC1);
  while (LL_ADC_REG_IsStopConversionOngoing(ADC1))
    ;
  LL_ADC_Disable(ADC1);
  aprs_ready1 = false;
  aprs_ready2 = false;
}

uint32_t BOARD_ADC_GetAvailableAPRS_DMA(void) {
  return (aprs_ready1 || aprs_ready2) ? APRS_BUFFER_SIZE : 0;
}

uint32_t BOARD_ADC_ReadAPRS_DMA(uint16_t *dest, uint32_t max_samples) {
  if (max_samples < APRS_BUFFER_SIZE) {
    return 0;
  }

  const volatile uint16_t *src = NULL;
  volatile bool *flag = NULL;

  if (aprs_ready1) {
    src = &adc_dma_buffer[0];
    flag = &aprs_ready1;
  } else if (aprs_ready2) {
    src = &adc_dma_buffer[APRS_BUFFER_SIZE];
    flag = &aprs_ready2;
  } else {
    return 0;
  }

  for (int i = 0; i < APRS_BUFFER_SIZE; i++) {
    dest[i] = src[i];
  }
  *flag = false;

  return APRS_BUFFER_SIZE;
}

void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent) {
  // Trigger a single injected conversion on CH8 (battery voltage).
  // Per RM 16.3.12.1: set JSWSTART while ADC is running; wait for JEOC.
  // The injected conversion interrupts the ongoing regular sequence and
  // resumes it afterwards — no need to stop DMA/regular group.
  LL_ADC_INJ_StartConversionSWStart(ADC1);

  uint32_t timeout = 100000;
  while (!LL_ADC_IsActiveFlag_JEOS(ADC1) && timeout--)
    ;

  *pVoltage =
      (uint16_t)LL_ADC_INJ_ReadConversionData12(ADC1, LL_ADC_INJ_RANK_1);
  *pCurrent = 0;

  LL_ADC_ClearFlag_JEOS(ADC1);
}

uint16_t BOARD_ADC_GetAPRS(void) {
  return adc_dma_buffer[0]; // CH9 is the only regular channel now
}

// ---------------------------------------------------------------------------
// DAC
// ---------------------------------------------------------------------------

void BOARD_DAC_Init(void) {
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_4, LL_GPIO_MODE_ANALOG);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_DAC1);
  LL_DAC_SetTriggerSource(DAC1, LL_DAC_CHANNEL_1, LL_DAC_TRIG_SOFTWARE);
  LL_DAC_SetOutputBuffer(DAC1, LL_DAC_CHANNEL_1, LL_DAC_OUTPUT_BUFFER_ENABLE);
  LL_DAC_Enable(DAC1, LL_DAC_CHANNEL_1);
}

void BOARD_DAC_SetValue(uint16_t value) {
  if (value > 4095)
    value = 4095;
  LL_DAC_ConvertData12RightAligned(DAC1, LL_DAC_CHANNEL_1, value);
  LL_DAC_TrigSWConversion(DAC1, LL_DAC_CHANNEL_1);
}

// ---------------------------------------------------------------------------
// Board init
// ---------------------------------------------------------------------------

void BOARD_Init(void) {
  BOARD_GPIO_Init();
  UART_Init(); // also enables SYSCFG clock, required before BOARD_ADC_Init
  LogC(LOG_C_BRIGHT_WHITE, "Init start");

  BOARD_TIM3_Init();
  BOARD_ADC_Init();
  BOARD_DAC_Init();

  LogC(LOG_C_BRIGHT_WHITE, "Flash init");
  PY25Q16_Init();
  LogC(LOG_C_BRIGHT_WHITE, "File system init");
  fs_init();
  LogC(LOG_C_BRIGHT_WHITE, "Squelch presets init");
  SQ_InitPresets();
  LogC(LOG_C_BRIGHT_WHITE, "Display init");
  ST7565_Init();
  LogC(LOG_C_BRIGHT_WHITE, "Backlight init");
  BACKLIGHT_InitHardware();
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

void BOARD_FlashlightToggle(void) { GPIO_TogglePin(GPIO_PIN_FLASHLIGHT); }
void BOARD_ToggleRed(bool on) { BK4819_ToggleGpioOut(BK4819_RED, on); }
void BOARD_ToggleGreen(bool on) { BK4819_ToggleGpioOut(BK4819_GREEN, on); }
