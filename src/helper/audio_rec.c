/* audio_rec.c — stub (audio I/O disabled due to RF noise) */

#include "audio_rec.h"

void AREC_Init(void) {}
void AREC_Update(void) {}
bool AREC_StartRecording(void) { return false; }
void AREC_StopRecording(void) {}
bool AREC_StartPlayback(void) { return false; }
void AREC_StopPlayback(void) {}
ARecInfo AREC_GetInfo(void) { return (ARecInfo){0}; }
uint32_t AREC_GetDurationMs(void) { return 0; }
