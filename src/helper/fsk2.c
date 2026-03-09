#include "fsk2.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RF_Write BK4819_WriteRegister
#define RF_Read BK4819_ReadRegister

#define CTC_ERR 0
#define CTC_IN 10
#define CTC_OUT 15

#define REG_37 0x1D00
#define REG_52 CTC_ERR << 12 | CTC_IN << 6 | CTC_OUT

// Синхрослово по умолчанию
#define FSK_SYNC_0 0x85
#define FSK_SYNC_1 0xCF
#define FSK_SYNC_2 0xAB
#define FSK_SYNC_3 0x45

const uint16_t REG_59 =
    (1 << 3) | ((8 - 1) << 4); // 4 байта sync, 8 байт preamble

uint16_t FSK_TXDATA[FSK_LEN];
uint16_t FSK_RXDATA[FSK_LEN];
bool gNewFskMessage;

void RF_EnterFsk() {
  // а) Останавливаем RX если был включен
  BK4819_WriteRegister(0x59, REG_59 & ~(1 << 12));
  // 1. Сначала отключаем FSK (софт-сброс согласно Application Notes)
  RF_Write(0x58, 0x0000); // Disable FSK

  // 2. Очищаем FIFO
  RF_Write(0x59, (1 << 15) | (1 << 14)); // Clear TX and RX FIFO
  SYSTICK_DelayMs(1);
  RF_Write(0x59, 0x0000);

  // 3. Настройка FSK параметров
  RF_Write(0x70, 0x00E0); // Enable Tone2 for FSK, Gain

  // 4. Настройка скорости 1200bps (регистр 0x72)
  RF_Write(0x72, 0x3065); // 1200bps

  // 5. Включение FSK режима (регистр 0x58)
  // Биты [15:13] = 000 - FSK1.2K TX режим
  // Биты [12:10] = 000 - FSK1.2K RX режим
  // Биты [5:4] = 00 - преамбула AA/55
  // Биты [3:1] = 000 - полоса для FSK1.2K
  // Бит [0] = 1 - включить FSK
  RF_Write(0x58, 0x00C1);

  // 6. Настройка CRC (отключено)
  // RF_Write(0x5C, 0x5665 & ~(1 << 6)); // disable crc
  RF_Write(0x5C, 0x5665); // disable crc

  // 7. Настройка длины пакета
  uint16_t length = FSK_LEN * 2 - 1;
  uint16_t reg_value = ((length & 0xFF) << 8) | //
                       ((length >> 8) << 5);    //
  RF_Write(0x5D, reg_value);

  // 8. Настройка FIFO thresholds
  RF_Write(0x5E, (64 << 3) | 4); // Almost empty=64, almost full=8

  // 9. Настройка девиации
  // BK4819_WriteRegister(0x40, 0x3000 + 1050); // 1050Hz deviation

  // 10. Настройка синхрослова (по умолчанию, но явно задаем)
  RF_Write(0x5A, (FSK_SYNC_0 << 8) | FSK_SYNC_1);
  RF_Write(0x5B, (FSK_SYNC_2 << 8) | FSK_SYNC_3);

  BK4819_WriteRegister(0x59, REG_59 | (1 << 12));

  SYSTICK_DelayMs(10);
}

void RF_ExitFsk() {
  RF_Write(0x58, 0x0000); // Disable FSK first
  RF_Write(0x70, 0x0000); // Disable Tone2
}

void RF_FskIdle() {
  RF_Write(0x3F, 0x0000); // tx sucs irq mask=0
  RF_Write(0x59, REG_59); // fsk_tx_en=0, fsk_rx_en=0
  BK4819_Idle();
}

bool RF_FskTransmit() {
  SYSTICK_DelayMs(100);

  // Включаем прерывание TX finished
  RF_Write(0x3F, 0x8000);

  // Очищаем TX FIFO
  RF_Write(0x59, REG_59 | 0x8000);
  RF_Write(0x59, REG_59);

  // Записываем данные в FIFO
  for (uint16_t i = 0; i < FSK_LEN; i++) {
    RF_Write(0x5F, FSK_TXDATA[i]);
  }

  SYSTICK_DelayMs(20);

  // Включаем передачу
  RF_Write(0x59, REG_59 | (1 << 11));

  // Ждем завершения передачи
  uint16_t rdata = 0;
  uint8_t cnt = 200;

  while (cnt && !(rdata & 0x1)) {
    SYSTICK_DelayMs(5);
    rdata = RF_Read(0x0C);
    cnt--;
  }

  // Очищаем прерывание и выключаем TX
  RF_Write(0x02, 0x0000);
  RF_Write(0x59, REG_59);

  return cnt;
}

typedef enum MsgStatus {
  READY,
  SENDING,
  RECEIVING,
} MsgStatus;

static uint16_t rxIdx;
static MsgStatus msgStatus = READY;

bool RF_FskReceive(uint16_t int_bits) {
  const bool sync = int_bits & BK4819_REG_02_FSK_RX_SYNC;
  const bool fifo_almost_full = int_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL;
  const bool finished = int_bits & BK4819_REG_02_FSK_RX_FINISHED;

  if (sync) {
    rxIdx = 0;
    msgStatus = RECEIVING;
    memset(FSK_RXDATA, 0, sizeof(FSK_RXDATA));

    // Очищаем RX FIFO при начале синхронизации
    uint16_t reg59 = RF_Read(BK4819_REG_59);
    RF_Write(BK4819_REG_59, reg59 | (1 << 14));
    RF_Write(BK4819_REG_59, reg59);
    Log("SYNC");
  }

  if (fifo_almost_full && msgStatus == RECEIVING) {
    for (uint16_t i = 0; i < 4 && rxIdx < FSK_LEN; i++) {
      FSK_RXDATA[rxIdx++] = RF_Read(BK4819_REG_5F);
    }
  }

  // Обработка завершения приема
  if (finished) {
    // Дочитываем все оставшиеся данные
    while (rxIdx < FSK_LEN) {
      uint16_t fifo_status = RF_Read(BK4819_REG_5E);
      uint16_t words_in_fifo = (fifo_status & 0x7) + 1;

      if (words_in_fifo == 0) {
        break;
      }

      for (uint16_t i = 0; i < words_in_fifo && rxIdx < FSK_LEN; i++) {
        FSK_RXDATA[rxIdx++] = RF_Read(BK4819_REG_5F);
      }
    }

    msgStatus = READY;

    // Важный момент: после FINISHED нужно перезапустить RX
    // согласно Application Notes, раздел 14
    uint16_t reg59 = RF_Read(BK4819_REG_59);

    // 1. Останавливаем RX
    RF_Write(BK4819_REG_59, reg59 & ~(1 << 12));
    SYSTICK_DelayMs(1);

    // 2. Очищаем FIFO
    RF_Write(BK4819_REG_59, reg59 | (1 << 14));
    SYSTICK_DelayMs(1);

    // 3. Запускаем RX снова
    RF_Write(BK4819_REG_59, reg59 | (1 << 12));

    // Отладочный вывод
    printf("%10u: RX [%d] ", Now(), rxIdx);
    for (uint16_t i = 0; i < FSK_LEN; ++i) {
      printf("%04X ", FSK_RXDATA[i]);
    }
    printf("\n");
    gNewFskMessage = true;

    rxIdx = 0;
    return true;
  }

  return false;
}
