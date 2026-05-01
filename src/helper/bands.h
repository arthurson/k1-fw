#ifndef BANDS_H
#define BANDS_H

#include "../inc/band.h"
#include <stdint.h>

#define MAX_BANDS 128
#define RANGES_STACK_SIZE 5

Band BANDS_ByFrequency(uint32_t f);
bool BANDS_InRange(uint32_t f, Band *b);
uint8_t BANDS_CalculateOutputPower(TXOutputPower power, uint32_t f);

void BANDS_Recreate();
void BANDS_RangeClear();
int8_t BANDS_RangeIndex();
bool BANDS_RangePush(Band r);
Band BANDS_RangePop(void);
Band *BANDS_RangePeek(void);

uint16_t BANDS_GetStepSize(Band *p);
uint32_t BANDS_GetSteps(Band *p);
uint32_t BANDS_GetF(Band *p, uint32_t channel);
uint32_t BANDS_GetChannel(Band *p, uint32_t f);

extern const Band DEFAULT_BAND;
extern const Band defaultBands[];

#endif // !BANDS_H
