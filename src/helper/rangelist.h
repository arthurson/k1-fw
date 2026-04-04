#ifndef RANGELIST_H
#define RANGELIST_H

#include "../inc/band.h"
#include <stdbool.h>
#include <stdint.h>

#define RANGE_SIZE_MAX 50

// Range entry for storage (simplified Band)
typedef struct {
  uint32_t start : 27;
  uint32_t end : 27;
  Step step : 4;
  uint8_t modulation : 4;
  uint8_t bw : 4;
  Radio radio : 2;
  Squelch squelch;
  uint8_t gainIndex : 5;
  bool allowTx : 1;
  char name[10];
} __attribute__((packed)) RangeEntry;

// Range list operations
uint16_t RLST_Size(void);
RangeEntry *RLST_Item(uint16_t i);
void RLST_Clear(void);
void RLST_Add(uint32_t start, uint32_t end, const char *name);
void RLST_Remove(uint16_t i);
void RLST_Update(uint16_t i, RangeEntry *entry);

// Persistence
bool RLST_SaveToFile(const char *filename);
bool RLST_LoadFromFile(const char *filename);
bool RLST_Save(void);
bool RLST_Load(void);

// Convert Band to RangeEntry
RangeEntry RLST_FromBand(const Band *band);
Band RLST_ToBand(const RangeEntry *entry);

// Explicit declaration to avoid implicit int
extern RangeEntry *RLST_Item(uint16_t i);

#endif
