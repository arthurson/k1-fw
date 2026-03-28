#ifndef FSK2_H
#define FSK2_H

#include <stdbool.h>
#include <stdint.h>

#define FSK_LEN 64

// Публичные буферы для FSK данных
extern uint16_t FSK_TXDATA[FSK_LEN];
extern uint16_t FSK_RXDATA[FSK_LEN];
extern bool gNewFskMessage;

// Функции управления RF режимом
void RF_Txon(void);
void RF_Rxon(void);
void RF_EnterFsk(void);
void RF_ExitFsk(void);
void RF_FskIdle(void);

// Основные функции FSK
bool RF_FskTransmit(void);
bool RF_FskReceive(uint16_t int_bits);

#endif // FSK2_H
