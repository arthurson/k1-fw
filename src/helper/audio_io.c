/* audio_io.c — audio I/O abstraction layer
 *
 * Architecture:
 *
 *   ADC CH9 (9600 Hz, DMA circular, ping-pong)
 *       │
 *       ▼
 *   AUDIO_IO_Update()   ← called from main loop
 *       │
 *       ├─► sink[0](buf, N)   e.g. oscilloscope / FFT
 *       ├─► sink[1](buf, N)   e.g. APRS decoder
 *       ├─► sink[2](buf, N)   e.g. flash recorder
 *       └─► sink[N](buf, N)   e.g. OOK decoder / custom
 *
 *   active_source()   ← called from TIM6/DAC DMA ISR
 *       │
 *       ▼
 *   DAC CH1 (PA4, 9600 Hz, DMA circular, ping-pong)
 *       │
 *       ▼
 *   Audio output (speaker / line-out)
 *
 * Output timing:
 *   TIM6 triggers DAC DMA at the same 9600 Hz as ADC input, so the
 *   output pipeline is symmetric with the input — convenient for loopback,
 *   playback of recorded audio, or AFSK/APRS TX.
 *
 *   The DAC DMA buffer is split into two halves (ping-pong).
 *   Each half fires an interrupt; the ISR calls the active source to refill
 *   the *idle* half while the DAC plays the *live* half.
 */

#include "audio_io.h"
#include "../board.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/PY32F071_HAL_Driver/Inc/py32f071_ll_bus.h"
#include "../external/PY32F071_HAL_Driver/Inc/py32f071_ll_dac.h"
#include "../external/PY32F071_HAL_Driver/Inc/py32f071_ll_dma.h"
#include "../external/PY32F071_HAL_Driver/Inc/py32f071_ll_system.h"
#include "../external/PY32F071_HAL_Driver/Inc/py32f071_ll_tim.h"
#include <string.h>

// ---------------------------------------------------------------------------
// External ADC state from board.c
// ---------------------------------------------------------------------------
extern volatile uint16_t adc_dma_buffer[2 * APRS_BUFFER_SIZE];
extern volatile bool aprs_ready1;
extern volatile bool aprs_ready2;

// ---------------------------------------------------------------------------
// DAC DMA output buffer
// Layout: [half-A: AUDIO_IO_DAC_BLOCK | half-B: AUDIO_IO_DAC_BLOCK]
// HT fires when half-A has been consumed → refill half-A
// TC fires when half-B has been consumed → refill half-B
// ---------------------------------------------------------------------------
static volatile uint16_t dac_dma_buf[2 * AUDIO_IO_DAC_BLOCK]
    __attribute__((aligned(4)));

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static AudioSink_Fn sinks[AUDIO_IO_MAX_SINKS];
static AudioSource_Fn active_source;

#ifdef AUDIO_IO_STATS
static AudioIO_Stats_t stats;
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Fill half of the DAC DMA buffer by calling the active source. */
static void refill_dac_half(uint16_t *half) {
  if (!active_source) {
    // No source — output mid-rail (silence)
    for (uint32_t i = 0; i < AUDIO_IO_DAC_BLOCK; i++)
      half[i] = 2048;
    return;
  }

  uint32_t written = active_source((uint16_t *)half, AUDIO_IO_DAC_BLOCK);

  if (written == 0) {
    // Source signalled end-of-stream
    active_source = NULL;
    for (uint32_t i = 0; i < AUDIO_IO_DAC_BLOCK; i++)
      half[i] = 2048;
#ifdef AUDIO_IO_STATS
    stats.dac_underruns++;
#endif
  } else if (written < AUDIO_IO_DAC_BLOCK) {
    // Partial fill — pad with last value to avoid click
    uint16_t last = half[written - 1];
    for (uint32_t i = written; i < AUDIO_IO_DAC_BLOCK; i++)
      half[i] = last;
  }
}

/** Dispatch one ADC block to all registered sinks. */
static void dispatch_to_sinks(const volatile uint16_t *src) {
  for (int i = 0; i < AUDIO_IO_MAX_SINKS; i++) {
    if (sinks[i])
      sinks[i]((const uint16_t *)src, AUDIO_IO_BLOCK_SIZE);
  }
#ifdef AUDIO_IO_STATS
  stats.blocks_dispatched++;
#endif
}

// ---------------------------------------------------------------------------
// TIM6 — drives DAC DMA at AUDIO_IO_FS_HZ
// ---------------------------------------------------------------------------

static void tim6_init(void) {
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);

  // 48 MHz / (PSC+1) / (ARR+1) = 9600 Hz
  // Same rate as TIM3 (ADC) → symmetric I/O pipeline
  LL_TIM_SetPrescaler(TIM6, 0);
  LL_TIM_SetAutoReload(TIM6, (48000000U / AUDIO_IO_FS_HZ) - 1); // 4999

  LL_TIM_SetCounterMode(TIM6, LL_TIM_COUNTERMODE_UP);
  LL_TIM_SetTriggerOutput(TIM6, LL_TIM_TRGO_UPDATE);
  LL_TIM_DisableMasterSlaveMode(TIM6);

  LL_TIM_EnableCounter(TIM6);
}

// ---------------------------------------------------------------------------
// DAC DMA (DMA1 Channel 3, remapped to DAC in SYSCFG)
// ---------------------------------------------------------------------------

static void dac_dma_init(void) {
  // Pre-fill both halves with silence
  for (uint32_t i = 0; i < 2 * AUDIO_IO_DAC_BLOCK; i++)
    dac_dma_buf[i] = 2048;

  // Use Channel 2 for DAC DMA.
  // On PY32F071 (like STM32F0 family) DMA channels 2 and 3 share a single
  // interrupt vector — DMA1_Channel2_3_IRQn / DMA1_Channel2_3_IRQHandler.
  // Channel 1 is already claimed by ADC (board.c).
  LL_DMA_DeInit(DMA1, LL_DMA_CHANNEL_2);

  LL_DMA_InitTypeDef d;
  LL_DMA_StructInit(&d);

  d.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
  d.PeriphOrM2MSrcAddress = (uint32_t)&DAC1->DHR12R1;
  d.MemoryOrM2MDstAddress = (uint32_t)dac_dma_buf;
  d.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_HALFWORD;
  d.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_HALFWORD;
  d.NbData = 2 * AUDIO_IO_DAC_BLOCK;
  d.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
  d.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
  d.Mode = LL_DMA_MODE_CIRCULAR;
  d.Priority = LL_DMA_PRIORITY_MEDIUM;

  LL_DMA_Init(DMA1, LL_DMA_CHANNEL_2, &d);

  LL_DMA_EnableIT_TC(DMA1, LL_DMA_CHANNEL_2);
  LL_DMA_EnableIT_HT(DMA1, LL_DMA_CHANNEL_2);
  LL_DMA_EnableIT_TE(DMA1, LL_DMA_CHANNEL_2);

  // Channels 2+3 share one vector on this MCU
  NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);

  // Remap DMA1 Ch2 → DAC1 (DMA_CHANNEL_MAP_DAC1 = 0x01 per datasheet)
  LL_SYSCFG_SetDMARemap(DMA1, LL_DMA_CHANNEL_2, LL_SYSCFG_DMA_MAP_DAC1);

  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_2);
}

static void dac_trigger_init(void) {
  // Switch DAC trigger from software (board.c) to TIM6 TRGO.
  // Must disable before changing trigger source (RM requirement).
  LL_DAC_Disable(DAC1, LL_DAC_CHANNEL_1);
  LL_DAC_SetTriggerSource(DAC1, LL_DAC_CHANNEL_1, LL_DAC_TRIG_EXT_TIM6_TRGO);
  // EnableTrigger is separate from SetTriggerSource on PY32/STM32.
  // Without this the DAC ignores TIM6 TRGO and never latches DHR → DOR.
  LL_DAC_EnableTrigger(DAC1, LL_DAC_CHANNEL_1);
  LL_DAC_EnableDMAReq(DAC1, LL_DAC_CHANNEL_1);
  LL_DAC_Enable(DAC1, LL_DAC_CHANNEL_1);
  // DAC needs ~1 us after enable before it can drive output (per RM)
  for (volatile int i = 0; i < 100; i++) {}
}

// ---------------------------------------------------------------------------
// DAC DMA ISR
// ---------------------------------------------------------------------------

// Shared ISR for DMA1 channels 2 and 3 (PY32F071 shares one vector for both).
// We only use channel 2 for DAC; channel 3 is free. Check CH2 flags only.
void DMA1_Channel2_3_IRQHandler(void) {
  if (LL_DMA_IsActiveFlag_HT2(DMA1)) {
    LL_DMA_ClearFlag_HT2(DMA1);
    // Half-A has been played out → refill half-A
    refill_dac_half((uint16_t *)&dac_dma_buf[0]);
  }

  if (LL_DMA_IsActiveFlag_TC2(DMA1)) {
    LL_DMA_ClearFlag_TC2(DMA1);
    // Half-B has been played out → refill half-B
    refill_dac_half((uint16_t *)&dac_dma_buf[AUDIO_IO_DAC_BLOCK]);
  }

  if (LL_DMA_IsActiveFlag_TE2(DMA1)) {
    LL_DMA_ClearFlag_TE2(DMA1);
    LogC(LOG_C_RED, "AUDIO_IO: DAC DMA error");
  }
}

// ---------------------------------------------------------------------------
// Public API — lifecycle
// ---------------------------------------------------------------------------

void AUDIO_IO_Init(void) {
  memset(sinks, 0, sizeof(sinks));
  active_source = NULL;

#ifdef AUDIO_IO_STATS
  memset(&stats, 0, sizeof(stats));
#endif

  tim6_init();
  dac_dma_init();
  dac_trigger_init();

  LogC(LOG_C_BRIGHT_WHITE, "AUDIO_IO: init ok (Fs=%u Hz, DAC block=%u)",
       AUDIO_IO_FS_HZ, AUDIO_IO_DAC_BLOCK);
}

// ---------------------------------------------------------------------------
// Public API — update (main loop)
// ---------------------------------------------------------------------------

void AUDIO_IO_Update(void) {
  // ADC half-A ready
  if (aprs_ready1) {
    aprs_ready1 = false;
    dispatch_to_sinks(&adc_dma_buffer[0]);
  }
  // ADC half-B ready
  if (aprs_ready2) {
    aprs_ready2 = false;
    dispatch_to_sinks(&adc_dma_buffer[APRS_BUFFER_SIZE]);
  }
}

// ---------------------------------------------------------------------------
// Public API — input sinks
// ---------------------------------------------------------------------------

bool AUDIO_IO_SinkRegister(AudioSink_Fn fn) {
  if (!fn)
    return false;

  // Check for duplicate
  for (int i = 0; i < AUDIO_IO_MAX_SINKS; i++) {
    if (sinks[i] == fn)
      return true; // already registered
  }

  // Find empty slot
  for (int i = 0; i < AUDIO_IO_MAX_SINKS; i++) {
    if (!sinks[i]) {
      sinks[i] = fn;
      return true;
    }
  }

  LogC(LOG_C_YELLOW, "AUDIO_IO: sink table full (%d slots)",
       AUDIO_IO_MAX_SINKS);
  return false;
}

void AUDIO_IO_SinkUnregister(AudioSink_Fn fn) {
  if (!fn)
    return;
  for (int i = 0; i < AUDIO_IO_MAX_SINKS; i++) {
    if (sinks[i] == fn)
      sinks[i] = NULL;
  }
}

void AUDIO_IO_SinkUnregisterAll(void) { memset(sinks, 0, sizeof(sinks)); }

// ---------------------------------------------------------------------------
// Public API — output source
// ---------------------------------------------------------------------------

void AUDIO_IO_SourceSet(AudioSource_Fn fn) {
  // Setting from main loop; DAC ISR reads active_source.
  // On Cortex-M0 pointer writes are atomic for aligned addresses,
  // so no critical section needed for a single pointer swap.
  active_source = fn;

  if (fn) {
    // Pre-fill both halves immediately so there's no silence gap
    // before the first ISR fires.
    refill_dac_half((uint16_t *)&dac_dma_buf[0]);
    refill_dac_half((uint16_t *)&dac_dma_buf[AUDIO_IO_DAC_BLOCK]);
  }
}

void AUDIO_IO_SourceClear(void) { AUDIO_IO_SourceSet(NULL); }

bool AUDIO_IO_SourceActive(void) { return active_source != NULL; }

// ---------------------------------------------------------------------------
// Public API — stats
// ---------------------------------------------------------------------------

#ifdef AUDIO_IO_STATS
const AudioIO_Stats_t *AUDIO_IO_GetStats(void) { return &stats; }
void AUDIO_IO_ResetStats(void) { memset(&stats, 0, sizeof(stats)); }
#endif

