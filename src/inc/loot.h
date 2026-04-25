#ifndef LOOT_H
#define LOOT_H

#include "channel.h"
#include <stdint.h>

// Упакованная Loot: 12 байт. Раскладка задана явно через bitfields.
//
// Поля squelch.type/squelch.value развёрнуты в squelch_type/squelch_value,
// потому что глобальный тип SQL (struct из двух uint8_t) занимает 2 байта,
// и без его расщепления в 12 байт не уложиться.
//
// lastTimeOpen и duration — секунды в uint16_t. Диапазон 65535 сек ≈ 18ч12м.
// Дольше — clamp/wrap (см. комментарии в lootlist.c).
//
// При сохранении в файл magic защищает от загрузки несовместимых данных.
typedef struct {
  uint32_t f; // частота, ед. 10 Гц

  uint16_t lastTimeOpen; // sec (Now()/1000), wrap каждые ~18ч
  uint16_t duration;     // sec, накопительно (clamp на 0xFFFF)

  // byte 9
  uint8_t modulation : 3; // 0..7 (нужно 0..4)
  uint8_t bw : 3;         // 0..5
  uint8_t radio : 2;      // 0..2

  // byte 10
  uint8_t gainIndex : 5;    // 0..31
  uint8_t squelch_type : 2; // 0..3
  uint8_t isCd : 1;

  // byte 11
  uint8_t squelch_value : 4; // 0..15
  uint8_t blacklist : 1;
  uint8_t whitelist : 1;
  uint8_t open : 1;
  uint8_t _pad : 1;

  // byte 12
  uint8_t code; // 0xFF = none
} Loot;

_Static_assert(sizeof(Loot) == 12, "Loot must be 12 bytes");

#endif // !LOOT_H
