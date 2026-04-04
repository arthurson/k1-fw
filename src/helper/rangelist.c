#include "rangelist.h"
#include "../driver/bk4829.h"
#include "../driver/lfs.h"
#include "../helper/storage.h"
#include "../inc/band.h"
#include <string.h>

static RangeEntry ranges[RANGE_SIZE_MAX] = {0};
static int16_t rangeIndex = -1;

uint16_t RLST_Size(void) { return rangeIndex + 1; }

RangeEntry *RLST_Item(uint16_t i) { return &ranges[i]; }

void RLST_Clear(void) { rangeIndex = -1; }

void RLST_Add(uint32_t start, uint32_t end, const char *name) {
  if (rangeIndex < RANGE_SIZE_MAX - 1) {
    rangeIndex++;
  } else {
    return;
  }

  ranges[rangeIndex] = (RangeEntry){
      .start = start,
      .end = end,
      .step = STEP_12_5kHz,
      .modulation = MOD_FM,
      .bw = 3, // BK4819_FILTER_BW_12k
      .radio = RADIO_BK4819,
      .squelch = {.type = 0, .value = 4},
      .gainIndex = 15,
      .allowTx = true,
  };

  if (name) {
    strncpy(ranges[rangeIndex].name, name, sizeof(ranges[rangeIndex].name));
  }
}

void RLST_Remove(uint16_t i) {
  if (!RLST_Size())
    return;
  for (; i < RLST_Size() - 1; ++i)
    ranges[i] = ranges[i + 1];
  rangeIndex--;
}

void RLST_Update(uint16_t i, RangeEntry *entry) {
  if (i < RLST_Size()) {
    ranges[i] = *entry;
  }
}

RangeEntry RLST_FromBand(const Band *band) {
  RangeEntry entry = {
      .start = band->start,
      .end = band->end,
      .step = band->step,
      .modulation = band->modulation,
      .bw = band->bw,
      .radio = band->radio,
      .squelch = band->squelch,
      .gainIndex = band->gainIndex,
      .allowTx = band->allowTx,
  };
  strncpy(entry.name, band->name, sizeof(entry.name));
  return entry;
}

Band RLST_ToBand(const RangeEntry *entry) {
  Band band = {0};
  band.start = entry->start;
  band.end = entry->end;
  band.step = entry->step;
  band.modulation = entry->modulation;
  band.bw = entry->bw;
  band.radio = entry->radio;
  band.squelch = entry->squelch;
  band.gainIndex = entry->gainIndex;
  band.allowTx = entry->allowTx;
  strncpy(band.name, entry->name, sizeof(band.name));
  return band;
}

// ============================================================================
// Persistence: save/load range list to/from file
// ============================================================================

bool RLST_SaveToFile(const char *filename) {
  uint16_t count = RLST_Size();
  if (count == 0) {
    return Storage_Save(filename, 0, &count, sizeof(count));
  }

  if (!Storage_Save(filename, 0, &count, sizeof(count))) {
    return false;
  }

  return Storage_SaveMultiple(filename, 1, ranges, sizeof(RangeEntry), count);
}

bool RLST_LoadFromFile(const char *filename) {
  uint16_t count = 0;

  if (!Storage_Load(filename, 0, &count, sizeof(count))) {
    return false;
  }

  if (count == 0 || count > RANGE_SIZE_MAX) {
    RLST_Clear();
    return true;
  }

  if (Storage_LoadMultiple(filename, 1, ranges, sizeof(RangeEntry), count)) {
    rangeIndex = count - 1;
    return true;
  }

  return false;
}

#define RANGE_DEFAULT_FILE "Ranges.rng"

bool RLST_Save(void) { return RLST_SaveToFile(RANGE_DEFAULT_FILE); }

bool RLST_Load(void) { return RLST_LoadFromFile(RANGE_DEFAULT_FILE); }
