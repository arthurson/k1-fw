/*
 * POCSAG 1200 baud через HARDWARE FSK-модем BK4819
 * (вместо TONE1/TONE2)
 */

#include "../driver/bk4829.h"
#include "../external/CMSIS/Device/PY32F071/Include/py32f071xB.h"
#include "fsk2.h" // <-- подключи fsk2.h
#include <stdint.h>
#include <string.h>

// -----------------------------------------------------------------------
// Параметры FSK (1200 бод, ±4.5 кГц)
// -----------------------------------------------------------------------
#define POCSAG_FSK_DATA_RATE 0x3065u // 1200 bps из fsk2.c
#define POCSAG_FSK_DEVIATION 0x1470u // твой хак + flat

// POCSAG sync (32 бита)
#define POCSAG_SYNC 0x7CD215D8u

// -----------------------------------------------------------------------
// BCH и построение батча — оставляем твои (без изменений)
// -----------------------------------------------------------------------
static uint32_t pocsag_bch_parity(uint32_t info21) { /* ... твой код ... */ }
static void pocsag_build_batch(uint32_t ric, const char *msg,
                               uint32_t batch[8][2]) { /* ... твой код ... */ }

// Теперь пакет храним как байты для FIFO (uint16_t, как в fsk2)
#define POCSAG_MAX_BYTES (18 * 4 + 4 + 16 * 4) // preamble + sync + batch
static uint8_t pocsag_packet_bytes[POCSAG_MAX_BYTES];
static uint16_t pocsag_packet_len; // в байтах

static void pocsag_build_packet(uint32_t ric, const char *msg) {
  uint32_t batch[8][2];
  pocsag_build_batch(ric, msg, batch);

  uint8_t *p = pocsag_packet_bytes;
  int pos = 0;

  // Короткая преамбула (модем добавит свою, но мы добавим ещё 0xAA)
  for (int i = 0; i < 32; i++)
    p[pos++] = 0xAA; // 32 байта 0xAA — максимум, что обычно ловится

  // Sync POCSAG (big-endian)
  p[pos++] = (POCSAG_SYNC >> 24) & 0xFF;
  p[pos++] = (POCSAG_SYNC >> 16) & 0xFF;
  p[pos++] = (POCSAG_SYNC >> 8) & 0xFF;
  p[pos++] = POCSAG_SYNC & 0xFF;

  // Батч (16 слов по 4 байта, big-endian, MSB first)
  for (int f = 0; f < 8; f++) {
    for (int w = 0; w < 2; w++) {
      uint32_t word = batch[f][w];
      p[pos++] = (word >> 24) & 0xFF;
      p[pos++] = (word >> 16) & 0xFF;
      p[pos++] = (word >> 8) & 0xFF;
      p[pos++] = word & 0xFF;
    }
  }
  pocsag_packet_len = pos;
}

// -----------------------------------------------------------------------
// Передача через FSK FIFO
// -----------------------------------------------------------------------
static void pocsag_transmit(void) {
  // Заполняем FSK_TXDATA (uint16_t, как требует RF_FskTransmit)
  for (uint16_t i = 0; i < FSK_LEN && i * 2 < pocsag_packet_len; i++) {
    uint16_t word =
        (pocsag_packet_bytes[i * 2] << 8) | pocsag_packet_bytes[i * 2 + 1];
    FSK_TXDATA[i] = word;
  }

  RF_EnterFsk(); // из fsk2.c — настраивает 1200 бод, sync и т.д.
  RF_FskTransmit(); // шлёт FIFO
  RF_ExitFsk();
}

// -----------------------------------------------------------------------
// Init / Deinit
// -----------------------------------------------------------------------
static void pocsag_tx_init(void) {
  BK4819_WriteRegister(0x40, POCSAG_FSK_DEVIATION); // deviation
  BK4819_WriteRegister(BK4819_REG_43, 0x3028); // flat audio (твой хак)

  // Остальное делает RF_EnterFsk()
  BK4819_EnterTxMute();
  BK4819_SetAF(BK4819_AF_MUTE);
  BK4819_EnableTXLink();
  BK4819_ExitTxMute();
}

static void pocsag_tx_deinit(void) {
  RF_ExitFsk();
  BK4819_EnterTxMute();
  BK4819_WriteRegister(0x40, 0x3000); // сброс deviation
}

// -----------------------------------------------------------------------
// Публичный API
// -----------------------------------------------------------------------
void POCSAG_Send(uint32_t ric, const char *msg) {
  pocsag_build_packet(ric, msg);
  pocsag_tx_init();
  __disable_irq();
  pocsag_transmit();
  __enable_irq();
  pocsag_tx_deinit();
}

void POCSAG_SendTest(void) { POCSAG_Send(95, "HELLO"); }
