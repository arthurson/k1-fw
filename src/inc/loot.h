#ifndef LOOT_H
#define LOOT_H

#include "channel.h"
#include <stdint.h>

// Упакованная Loot: 16 байт. Раскладка задана явно через bitfields.
//
// Поля squelch.type/squelch.value развёрнуты в squelch_type/squelch_value,
// потому что глобальный тип SQL (struct из двух uint8_t) занимает 2 байта,
// и без его расщепления в 16 байт не уложиться.
//
// При сохранении в файл magic защищает от загрузки несовместимых данных.
typedef struct {
  uint32_t f;            // частота, ед. 10 Гц
  uint32_t lastTimeOpen; // ms (Now())
  uint32_t duration;     // ms

  // byte 13
  uint8_t modulation : 3; // 0..7 (нужно 0..4)
  uint8_t bw : 3;         // 0..5
  uint8_t radio : 2;      // 0..2

  // byte 14
  uint8_t gainIndex : 5;    // 0..31
  uint8_t squelch_type : 2; // 0..3
  uint8_t isCd : 1;

  // byte 15
  uint8_t squelch_value : 4; // 0..15
  uint8_t blacklist : 1;
  uint8_t whitelist : 1;
  uint8_t open : 1;
  uint8_t _pad : 1;

  // byte 16
  uint8_t code; // 0xFF = none
} Loot;

_Static_assert(sizeof(Loot) == 16, "Loot must be 16 bytes");

#endif // !LOOT_H
