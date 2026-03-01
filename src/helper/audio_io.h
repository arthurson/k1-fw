/* audio_io.h — abstraction layer over ADC/DAC audio I/O
 *
 * INPUT  (ADC DMA, Fs = 9600 Hz, 12-bit, CH9):
 *   Multiple sinks can subscribe to the raw sample stream.
 *   Each sink receives a pointer to a block of APRS_BUFFER_SIZE samples
 *   and must return quickly (no blocking, no heavy math inside the callback).
 *
 * OUTPUT (DAC, PA4, 12-bit):
 *   A single active source provides samples on demand.
 *   The DAC is fed by TIM6-triggered DMA so the source is called from
 *   the DAC-DMA half/complete interrupts, not from the main loop.
 *
 * Usage example — register an oscilloscope sink:
 *
 *   static void osc_sink(const uint16_t *buf, uint32_t n) {
 *       for (uint32_t i = 0; i < n; i++) push_sample(buf[i]);
 *   }
 *
 *   AUDIO_IO_SinkRegister(osc_sink);
 *
 * Usage example — play a sine wave via DAC:
 *
 *   static uint32_t sine_source(uint16_t *buf, uint32_t n) {
 *       for (uint32_t i = 0; i < n; i++)
 *           buf[i] = 2048 + 2047 * sin_lut[phase++ % LUT_SIZE];
 *       return n;  // returning 0 signals "source finished"
 *   }
 *
 *   AUDIO_IO_SourceSet(sine_source);
 */

#ifndef AUDIO_IO_H
#define AUDIO_IO_H

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// ADC sample rate — derived from board.c TIM3: 48 MHz / (PSC+1) / (ARR+1)
//   PSC = 0, ARR = 4999  →  48 000 000 / 5000 = 9600 Hz
#define AUDIO_IO_FS_HZ 9600U

// Block size matches the DMA ping-pong half — see board.c APRS_BUFFER_SIZE
// Sinks always receive exactly this many samples per call.
#define AUDIO_IO_BLOCK_SIZE APRS_BUFFER_SIZE

// Maximum number of simultaneously registered input sinks.
// Raise if you need more (costs one function pointer per slot).
#define AUDIO_IO_MAX_SINKS 6

// DAC output block size (samples fed to DMA per half-transfer interrupt).
// Smaller = lower latency, larger = less CPU overhead.
#define AUDIO_IO_DAC_BLOCK 64U

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/**
 * AudioSink — consumer of ADC samples.
 *
 * Called from AUDIO_IO_Update() in the main loop whenever a DMA half is ready.
 * @param buf   Pointer to AUDIO_IO_BLOCK_SIZE 12-bit ADC samples [0..4095].
 * @param count Always AUDIO_IO_BLOCK_SIZE (provided for safety/future).
 */
typedef void (*AudioSink_Fn)(const uint16_t *buf, uint32_t count);

/**
 * AudioSource — producer of DAC samples.
 *
 * Called from DAC-DMA ISR to refill the output buffer.
 * Must be ISR-safe: no blocking, no UART, no flash reads.
 *
 * @param buf   Output buffer to fill with 12-bit DAC values [0..4095].
 * @param n     Number of samples requested (always AUDIO_IO_DAC_BLOCK).
 * @return      Number of samples actually written.
 *              Return 0 to signal end-of-stream → source is auto-cleared.
 */
typedef uint32_t (*AudioSource_Fn)(uint16_t *buf, uint32_t n);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * Initialize the audio I/O layer.
 * Must be called after BOARD_ADC_Init() and BOARD_DAC_Init().
 * Sets up TIM6 + DAC-DMA for output; ADC DMA is already running from board.c.
 */
void AUDIO_IO_Init(void);

/**
 * Call from the main loop. Drains the ADC DMA flags and dispatches to sinks.
 * Should be called as frequently as possible (every loop iteration).
 */
void AUDIO_IO_Update(void);

// ---------------------------------------------------------------------------
// Input (ADC → sinks)
// ---------------------------------------------------------------------------

/**
 * Register a sink. Returns true on success, false if the table is full.
 * Registering the same function twice is a no-op (returns true).
 */
bool AUDIO_IO_SinkRegister(AudioSink_Fn fn);

/**
 * Unregister a previously registered sink. Safe to call with NULL or unknown
 * fn.
 */
void AUDIO_IO_SinkUnregister(AudioSink_Fn fn);

/**
 * Unregister all sinks at once.
 */
void AUDIO_IO_SinkUnregisterAll(void);

// ---------------------------------------------------------------------------
// Output (source → DAC)
// ---------------------------------------------------------------------------

/**
 * Set the active DAC source. Replaces any previously set source immediately.
 * Pass NULL to stop DAC output (DAC will output mid-rail 2048).
 */
void AUDIO_IO_SourceSet(AudioSource_Fn fn);

/**
 * Clear the current source (same as AUDIO_IO_SourceSet(NULL)).
 */
void AUDIO_IO_SourceClear(void);

/**
 * Returns true if a source is currently active.
 */
bool AUDIO_IO_SourceActive(void);

// ---------------------------------------------------------------------------
// Stats / diagnostics (optional, can be compiled out)
// ---------------------------------------------------------------------------

#ifdef AUDIO_IO_STATS
typedef struct {
  uint32_t blocks_dispatched; // total ADC blocks sent to sinks
  uint32_t dac_underruns;     // times source returned 0 samples unexpectedly
  uint32_t sink_overflows;    // times a sink was skipped (shouldn't happen)
} AudioIO_Stats_t;

const AudioIO_Stats_t *AUDIO_IO_GetStats(void);
void AUDIO_IO_ResetStats(void);
#endif

#endif // AUDIO_IO_H
