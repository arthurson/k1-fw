#ifndef LOOTLIST_HELPER_H

#define LOOTLIST_HELPER_H

#include "../inc/channel.h"
#include "../inc/loot.h"
#include <stdbool.h>
#include <stdint.h>

#define LOOT_SIZE_MAX 50

int16_t LOOT_IndexOf(Loot *loot);
void LOOT_BlacklistLast();
void LOOT_WhitelistLast();
Loot *LOOT_Get(uint32_t f);
Loot *LOOT_AddEx(uint32_t f, bool reuse);
Loot *LOOT_Add(uint32_t f);
void LOOT_Remove(uint16_t i);
void LOOT_Clear();
void LOOT_Standby();
uint16_t LOOT_Size();
Loot *LOOT_Item(uint16_t i);
void LOOT_UpdateEx(Loot *loot, Measurement *msm);
void LOOT_Update(Measurement *msm);
void LOOT_Replace(Measurement *loot, uint32_t f);

void LOOT_Sort(bool (*compare)(const Loot *a, const Loot *b), bool reverse);

bool LOOT_SortByLastOpenTime(const Loot *a, const Loot *b);
bool LOOT_SortByDuration(const Loot *a, const Loot *b);
bool LOOT_SortByF(const Loot *a, const Loot *b);
bool LOOT_SortByBlacklist(const Loot *a, const Loot *b);

void LOOT_RemoveBlacklisted(void);
CH LOOT_ToCh(const Loot *loot);

// Persistence: save/load loot list to/from file
bool LOOT_SaveToFile(const char *filename);
bool LOOT_LoadFromFile(const char *filename);
bool LOOT_Save(void);
bool LOOT_Load(void);

extern Loot *gLastActiveLoot;
extern int16_t gLastActiveLootIndex;

#endif
