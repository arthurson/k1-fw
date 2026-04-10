#include "backlight.h"
#include "../external/printf/printf.h"
#include "../settings.h"
#include "gpio.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_system.h"
#include "py32f071_ll_tim.h"
#include "systick.h"

#ifdef ENABLE_FEAT_F4HWN
#include "audio.h"
#include "driver/system.h"
#include "misc.h"
#endif

// 100 kHz вместо 25 kHz — сдвигаем гармоники PWM выше RF диапазона
#define PWM_FREQ 100000
#define DUTY_CYCLE_LEVELS 64

#define DUTY_CYCLE_ON_VALUE GPIO_PIN_MASK(GPIO_PIN_BACKLIGHT)
#define DUTY_CYCLE_OFF_VALUE (DUTY_CYCLE_ON_VALUE << 16)

#define TIMx TIM7
#define DMA_CHANNEL LL_DMA_CHANNEL_7

static uint32_t dutyCycle[DUTY_CYCLE_LEVELS];

// this is decremented once every 500ms
uint16_t gBacklightCountdown_500ms = 0;
bool backlightOn;

const uint8_t value[] = {0, 3, 6, 9, 15, 24, 38, 62, 100, 159, 255};

uint16_t gSleepModeCountdown_500ms = 0;

void BACKLIGHT_InitHardware() {
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM7);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

  LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_TIM7);
  LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_TIM7);

  // 48 MHz / ((1 + PSC) * (1 + ARR)) == PWM_freq * levels
  LL_TIM_SetPrescaler(TIMx, 0);
  LL_TIM_SetAutoReload(TIMx,
                       SystemCoreClock / PWM_FREQ / DUTY_CYCLE_LEVELS - 1);
  LL_TIM_EnableARRPreload(TIMx);
  LL_TIM_EnableDMAReq_UPDATE(TIMx);
  LL_TIM_EnableUpdateEvent(TIMx);

  LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
  LL_SYSCFG_SetDMARemap(DMA1, DMA_CHANNEL, LL_SYSCFG_DMA_MAP_TIM7_UP);

  LL_DMA_ConfigTransfer(DMA1, DMA_CHANNEL,                //
                        LL_DMA_DIRECTION_MEMORY_TO_PERIPH //
                            | LL_DMA_MODE_CIRCULAR        //
                            | LL_DMA_PERIPH_NOINCREMENT   //
                            | LL_DMA_MEMORY_INCREMENT     //
                            | LL_DMA_PDATAALIGN_WORD      //
                            | LL_DMA_MDATAALIGN_WORD      //
                            | LL_DMA_PRIORITY_HIGH        //
  );

  LL_DMA_SetMemoryAddress(DMA1, DMA_CHANNEL, (uint32_t)dutyCycle);
  LL_DMA_SetPeriphAddress(DMA1, DMA_CHANNEL,
                          (uint32_t)(&GPIO_PORT(GPIO_PIN_BACKLIGHT)->BSRR));
  LL_DMA_SetDataLength(DMA1, DMA_CHANNEL, sizeof(dutyCycle) / sizeof(uint32_t));
}

void BACKLIGHT_TurnOn(void) {

  if (gSettings.backlight == 0) {
    BACKLIGHT_TurnOff();
    return;
  }

  backlightOn = true;

  BACKLIGHT_SetBrightness(gSettings.brightness);

  gBacklightCountdown_500ms = 1 + (BL_TIME_VALUES[gSettings.backlight]) * 2;
}

void BACKLIGHT_TurnOff() {
  BACKLIGHT_SetBrightness(gSettings.brightnessLow);
  gBacklightCountdown_500ms = 0;
  backlightOn = false;
}

bool BACKLIGHT_IsOn() { return backlightOn; }

static uint8_t currentBrightness = 0;

void BACKLIGHT_SetBrightness(uint8_t brigtness) {
  // printf("BL: %d\n", brigtness);

  if (currentBrightness == brigtness) {
    return;
  }

  if (0 == brigtness) {
    LL_TIM_DisableCounter(TIMx);
    LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
    SYSTICK_DelayUs(1);
    GPIO_TurnOffBacklight();
  } else {
    const uint32_t level =
        (uint32_t)(value[brigtness]) * DUTY_CYCLE_LEVELS / 255;
    if (level >= DUTY_CYCLE_LEVELS) {
      LL_TIM_DisableCounter(TIMx);
      LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
      GPIO_TurnOnBacklight();
    } else {
      for (uint32_t i = 0; i < DUTY_CYCLE_LEVELS; i++) {
        dutyCycle[i] = i < level ? DUTY_CYCLE_ON_VALUE : DUTY_CYCLE_OFF_VALUE;
      }

      if (!LL_TIM_IsEnabledCounter(TIMx)) {
        LL_DMA_EnableChannel(DMA1, DMA_CHANNEL);
        LL_TIM_EnableCounter(TIMx);
      }
    }
  }

  currentBrightness = brigtness;
}

uint8_t BACKLIGHT_GetBrightness(void) { return currentBrightness; }

void BACKLIGHT_UpdateTimer() {
  if (gBacklightCountdown_500ms) {
    gBacklightCountdown_500ms--;
    return;
  }
  BACKLIGHT_TurnOff();
}
