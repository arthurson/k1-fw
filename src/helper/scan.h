#ifndef SCAN_H
#define SCAN_H

#include "../helper/measurements.h"
#include "../helper/scancommand.h"
#include "../inc/band.h"
#include <stdbool.h>
#include <stdint.h>

// Состояния сканирования (unified для частот и VFO)
typedef enum {
  SCAN_STATE_IDLE, // Ничего не делаем / ждём команды
  SCAN_STATE_SWITCHING, // Переключаем частоту/VFO
  SCAN_STATE_DECIDING,  // Ждём SQL_DELAY и проверяем squelch
  SCAN_STATE_LISTENING  // Squelch открыт, слушаем
} ScanState;

// Режимы сканирования
typedef enum {
  SCAN_MODE_SINGLE,     // Одна частота (мониторинг)
  SCAN_MODE_FREQUENCY,  // Частотное сканирование
  SCAN_MODE_CHANNEL,    // Канальное сканирование
  SCAN_MODE_ANALYSER,   // Анализатор спектра
  SCAN_MODE_MULTIWATCH, // Мультивотч VFO (из radio.c)
  SCAN_MODE_NONE,
} ScanMode;

// Контекст сканирования
typedef struct {
  // Состояние
  ScanState state;
  ScanMode mode;
  uint32_t stateEnteredAt;

  // Диапазон сканирования
  uint32_t currentF;
  uint32_t startF;
  uint32_t endF;
  uint16_t stepF;
  bool rangeActive;

  // Squelch и измерения
  uint32_t squelchLevel;
  Measurement measurement;
  bool isOpen; // Единственный флаг состояния squelch

  // Таймауты
  uint32_t listenTimeout;
  uint32_t closeTimeout;

  // Параметры
  uint32_t scanDelayUs; // Warmup delay
  uint32_t sqlDelayMs;  // Deciding delay

  // Статистика
  uint32_t scanCycles;
  uint32_t currentCps;
  uint32_t lastCpsTime;

  uint8_t scanCyclesSql;

  // Команды
  SCMD_Context *cmdCtx;

  // Multiwatch (для VFO сканирования)
  int8_t currentVfoIndex;

} ScanContext;

// API
void SCAN_Init(bool multiband);
void SCAN_SetMode(ScanMode mode);
ScanMode SCAN_GetMode(void);

void SCAN_Check(void); // Главный update loop

// Команды
void SCAN_LoadCommandFile(const char *filename);
void SCAN_SetCommandMode(bool enabled);
bool SCAN_IsCommandMode(void);
void SCAN_CommandForceNext(void);

// Управление диапазоном
void SCAN_setBand(Band b);
void SCAN_setStartF(uint32_t f);
void SCAN_setEndF(uint32_t f);
void SCAN_setRange(uint32_t fs, uint32_t fe);

// Действия
void SCAN_Next(void);
void SCAN_NextBlacklist(void);
void SCAN_NextWhitelist(void);

// Параметры
void SCAN_SetDelay(uint32_t delay);
uint32_t SCAN_GetDelay(void);
uint32_t SCAN_GetCps(void);

// Команды
SCMD_Command *SCAN_GetCurrentCommand(void);
SCMD_Command *SCAN_GetNextCommand(void);
uint16_t SCAN_GetCommandIndex(void);
uint16_t SCAN_GetCommandCount(void);
uint16_t SCAN_GetSquelchLevel();

void SCAN_HandleInterrupt(uint16_t int_bits);

bool SCAN_IsSqOpen(void);

extern const char *SCAN_MODE_NAMES[];
extern const char *SCAN_STATE_NAMES[];

#endif
