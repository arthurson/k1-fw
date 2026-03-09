#include "scan.h"
#include "../apps/apps.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/lootlist.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/spectrum.h"
#include "bands.h"
#include "measurements.h"
#include <stdbool.h>

// ============================================================================
// Глобальный контекст
// ============================================================================

static ScanContext scan = {
    .state = SCAN_STATE_IDLE,
    .mode = SCAN_MODE_SINGLE,
    .scanDelayUs = 1200,
    .sqlDelayMs = SQL_DELAY,
    .squelchLevel = 0,
    .isOpen = false,
    .rangeActive = false,
    .currentVfoIndex = -1,
    .cmdCtx = NULL,
};

static SCMD_Context cmdctx;

const char *SCAN_MODE_NAMES[] = {
    [SCAN_MODE_SINGLE] = "VFO",
    [SCAN_MODE_FREQUENCY] = "Scan",
    [SCAN_MODE_CHANNEL] = "CH Scan",
    [SCAN_MODE_ANALYSER] = "Analyser",
    [SCAN_MODE_MULTIWATCH] = "MultiWatch",
};

const char *SCAN_STATE_NAMES[] = {
    [SCAN_STATE_IDLE] = "Idle",
    [SCAN_STATE_SWITCHING] = "Sw",
    [SCAN_STATE_DECIDING] = "Decd",
    [SCAN_STATE_LISTENING] = "Lsn",
};

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void UpdateCPS(void) {
  uint32_t now = Now();
  uint32_t elapsed = now - scan.lastCpsTime;

  if (elapsed >= 1000) {
    if (elapsed > 0) {
      scan.currentCps = (scan.scanCycles * 1000) / elapsed;
    }
    scan.lastCpsTime = now;
    scan.scanCycles = 0;
  }
}

static void ChangeState(ScanState newState) {
  if (scan.state != newState) {
    /* Log("%s->%s %u", SCAN_STATE_NAMES[scan.state],
       SCAN_STATE_NAMES[newState], vfo->msm.f); */
    scan.state = newState;
    scan.stateEnteredAt = Now();
  }
}

static void ApplyBandSettings(void) {
  vfo->msm.f = gCurrentBand.start;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, vfo->msm.f, false);
  RADIO_SetParam(ctx, PARAM_STEP, gCurrentBand.step, false);
  RADIO_ApplySettings(ctx);
  SP_Init(&gCurrentBand);

  LogC(LOG_C_BRIGHT_YELLOW, "[SCAN] Bounds: %u .. %u", gCurrentBand.start,
       gCurrentBand.end);

  if (gLastActiveLoot && !BANDS_InRange(gLastActiveLoot->f, &gCurrentBand)) {
    gLastActiveLoot = NULL;
  }
}

static void SetScanRange(uint32_t start, uint32_t end, uint16_t step) {
  scan.startF = start;
  scan.endF = end;
  scan.currentF = start;
  scan.stepF = step;
  scan.rangeActive = true;

  Log("[SCAN] Range: %u-%u Hz, step=%u", start, end, step);

  ChangeState(SCAN_STATE_SWITCHING);
}

// ============================================================================
// Обработка команд
// ============================================================================

static void ApplyCommand(SCMD_Command *cmd) {
  if (!cmd)
    return;

  Log("[SCAN] CMD: type=%d, start=%lu, end=%lu", cmd->type, cmd->start,
      cmd->end);

  switch (cmd->type) {
  case SCMD_CHANNEL:
    SetScanRange(cmd->start, cmd->start, 0);
    break;

  case SCMD_RANGE: {
    uint32_t step = cmd->step;
    SetScanRange(cmd->start, cmd->end, step);
    break;
  }

  case SCMD_PAUSE:
    Log("[SCAN] Pause %u ms", cmd->dwell_ms);
    SYSTICK_DelayMs(cmd->dwell_ms);
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
    }
    break;

  case SCMD_JUMP:
    Log("[SCAN] Jump");
    break;

  case SCMD_MARKER:
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
    }
    break;

  default:
    Log("[SCAN] Unknown CMD: %d", cmd->type);
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
    }
    break;
  }
}

static void HandleEndOfRange(void) {
  if (scan.cmdCtx) {
    // Командный режим - следующая команда
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
      Log("[SCAN] Command sequence restarted");
    }
    scan.rangeActive = false;
    ChangeState(SCAN_STATE_IDLE);
  } else {
    // Обычный режим - возврат к началу
    scan.currentF = scan.startF;
    ChangeState(SCAN_STATE_SWITCHING);
    SP_Begin();
  }
  gRedrawScreen = true;
}

// ============================================================================
// State Machine - Частотное сканирование
// ============================================================================

static void HandleStateIdle(void) {
  // Ожидание команды в командном режиме
  if (scan.cmdCtx && !scan.rangeActive) {
    SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
    if (cmd) {
      ApplyCommand(cmd);
    }
  }
}

static void HandleStateSwitching(void) {

  if (scan.currentF > scan.endF) {
    HandleEndOfRange();
    return;
  }

  RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
  RADIO_ApplySettings(ctx);
  SYSTICK_DelayUs(scan.scanDelayUs);
  scan.measurement.rssi = RADIO_GetRSSI(ctx);
  scan.measurement.f = scan.currentF;

  scan.scanCycles++;
  scan.scanCyclesSql++;
  if (scan.scanCyclesSql >= 32) {
    scan.squelchLevel--;
    scan.scanCyclesSql = 0;
  }
  UpdateCPS();

  // Быстрая программная проверка для анализатора
  if (scan.mode == SCAN_MODE_ANALYSER) {
    SP_AddPoint(&scan.measurement);
    scan.currentF += scan.stepF;
    ChangeState(SCAN_STATE_SWITCHING);
    return;
  }

  // Программная грубая фильтрация
  if (!scan.squelchLevel && scan.measurement.rssi) {
    scan.squelchLevel = scan.measurement.rssi - 1;
  }

  bool programOpen = scan.measurement.rssi >= scan.squelchLevel;
  if (programOpen && gSettings.skipGarbageFrequencies &&
      scan.currentF % 650000 == 0) {
    programOpen = false;
  }

  if (programOpen) {
    // Потенциально есть сигнал - точная проверка
    // scan.currentF += scan.stepF; // WTF?!

    ChangeState(SCAN_STATE_DECIDING);
  } else {
    // Точно нет сигнала - на следующую частоту
    scan.measurement.open = false;
    SP_AddPoint(&scan.measurement);
    scan.currentF += scan.stepF;
    ChangeState(SCAN_STATE_SWITCHING);
  }
}

static void HandleStateDeciding(void) {
  if (Now() - scan.stateEnteredAt >= scan.sqlDelayMs) {
    // Аппаратная проверка squelch
    RADIO_UpdateSquelch(gRadioState);
    scan.isOpen = vfo->is_open;

    scan.measurement.open = scan.isOpen;
    LOOT_Update(&scan.measurement);
    SP_AddPoint(&scan.measurement);

    if (scan.isOpen) {
      // Сигнал подтверждён
      gRedrawScreen = true;

      // Auto-whitelist
      if (scan.cmdCtx) {
        SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
        if (cmd && (cmd->flags & SCMD_FLAG_AUTO_WHITELIST)) {
          LOOT_WhitelistLast();
          Log("[SCAN] Auto-whitelisted %u Hz", scan.currentF);
        }
      }

      ChangeState(SCAN_STATE_LISTENING);
    } else {
      // Ложное срабатывание - повышаем порог
      scan.squelchLevel++;
      ChangeState(SCAN_STATE_SWITCHING);
    }
  }
}

static void HandleStateListening(void) {
  // Обновляем состояние squelch
  RADIO_UpdateSquelch(gRadioState);
  SP_AddPoint(&scan.measurement);
  bool wasOpen = scan.isOpen;
  scan.isOpen = vfo->is_open;

  if (wasOpen != scan.isOpen) {
    gRedrawScreen = true;
  }

  uint32_t elapsed = Now() - scan.stateEnteredAt;

  if (scan.isOpen) {
    // Проверка таймаута прослушивания
    if (elapsed >= SCAN_TIMEOUTS[gSettings.sqOpenedTimeout]) {
      Log("[SCAN] Listen timeout");
      ChangeState(SCAN_STATE_SWITCHING);
      gRedrawScreen = true;
    }
  } else {
    // Squelch закрылся - ждём таймаут закрытия
    if (elapsed >= SCAN_TIMEOUTS[gSettings.sqClosedTimeout]) {
      Log("[SCAN] Close timeout");
      ChangeState(SCAN_STATE_SWITCHING);
      gRedrawScreen = true;
    }
  }
}

// ============================================================================
// State Machine - Single VFO (мониторинг)
// ============================================================================

static uint32_t radioTimer = 0;
static void HandleModeSingle(void) {

  scan.measurement.rssi = vfo->msm.rssi;
  scan.measurement.noise = vfo->msm.noise;
  scan.measurement.glitch = vfo->msm.glitch;
  scan.measurement.snr = vfo->msm.snr;

  RADIO_CheckAndSaveVFO(gRadioState);

  if (Now() - radioTimer >= SQL_DELAY) {
    RADIO_UpdateSquelch(gRadioState);
    SP_ShiftGraph(-1); // TODO: second buffer =)
    SP_AddGraphPoint(&scan.measurement);
    radioTimer = Now();
  }
}

// ============================================================================
// Главная функция
// ============================================================================

void SCAN_Check(void) {
  if (scan.mode == SCAN_MODE_NONE) {
    return;
  }
  // Multiwatch обрабатывается отдельно
  RADIO_UpdateMultiwatch(gRadioState);

  // Single mode - отдельная логика
  if (scan.mode == SCAN_MODE_SINGLE) {
    HandleModeSingle();
    return;
  }

  // State machine для частотного сканирования
  switch (scan.state) {
  case SCAN_STATE_IDLE:
    HandleStateIdle();
    break;

  case SCAN_STATE_SWITCHING:
    HandleStateSwitching();
    break;

  case SCAN_STATE_DECIDING:
    HandleStateDeciding();
    break;

  case SCAN_STATE_LISTENING:
    HandleStateListening();
    break;
  }
}

// ============================================================================
// API функции
// ============================================================================

void SCAN_SetMode(ScanMode mode) {
  if (scan.cmdCtx && mode != scan.mode) {
    SCAN_SetCommandMode(false);
  }

  scan.mode = mode;
  Log("[SCAN] mode=%s", SCAN_MODE_NAMES[scan.mode]);

  scan.scanCycles = 0;
  scan.squelchLevel = 0;
  ChangeState(SCAN_STATE_IDLE);

  switch (mode) {
  case SCAN_MODE_NONE:
    break;
  case SCAN_MODE_SINGLE:
    scan.rangeActive = false;
    break;

  case SCAN_MODE_FREQUENCY:
  case SCAN_MODE_ANALYSER:
    ApplyBandSettings();
    SetScanRange(gCurrentBand.start, gCurrentBand.end,
                 StepFrequencyTable[gCurrentBand.step]);
    break;

  case SCAN_MODE_CHANNEL:
    // TODO: channel mode
    break;

  case SCAN_MODE_MULTIWATCH:
    // Handled by RADIO_UpdateMultiwatch
    break;
  }
}

ScanMode SCAN_GetMode(void) { return scan.mode; }

void SCAN_Init(bool multiband) {
  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;

  ApplyBandSettings();
  BK4819_WriteRegister(BK4819_REG_3F, 0);
  vfo->is_open = false;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
}

void SCAN_setBand(Band b) {
  gCurrentBand = b;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(gCurrentBand.start, gCurrentBand.end,
                 StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setStartF(uint32_t f) {
  gCurrentBand.start = f;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(f, gCurrentBand.end, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setEndF(uint32_t f) {
  gCurrentBand.end = f;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(gCurrentBand.start, f, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.start = fs;
  gCurrentBand.end = fe;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    SetScanRange(fs, fe, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_Next(void) {
  vfo->is_open = false;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  ChangeState(SCAN_STATE_SWITCHING);
}

void SCAN_NextBlacklist(void) {
  LOOT_BlacklistLast();
  SCAN_Next();
}

void SCAN_NextWhitelist(void) {
  LOOT_WhitelistLast();
  SCAN_Next();
}

void SCAN_SetDelay(uint32_t delay) { scan.scanDelayUs = delay; }

uint32_t SCAN_GetDelay(void) { return scan.scanDelayUs; }

uint32_t SCAN_GetCps(void) { return scan.currentCps; }

// ============================================================================
// Командный режим
// ============================================================================

void SCAN_LoadCommandFile(const char *filename) {
  if (!scan.cmdCtx) {
    scan.cmdCtx = &cmdctx;
    memset(scan.cmdCtx, 0, sizeof(SCMD_Context));
  }

  SCMD_DebugDumpFile(filename);

  if (SCMD_Init(scan.cmdCtx, filename)) {
    scan.mode = SCAN_MODE_FREQUENCY;
    scan.rangeActive = false;
    ChangeState(SCAN_STATE_IDLE);
    Log("[SCAN] Loaded command file: %s", filename);
  } else {
    scan.cmdCtx = NULL;
    Log("[SCAN] Failed to load: %s", filename);
  }
}

void SCAN_SetCommandMode(bool enabled) {
  if (!enabled && scan.cmdCtx) {
    SCMD_Close(scan.cmdCtx);
    scan.cmdCtx = NULL;
    scan.rangeActive = false;
    Log("[SCAN] Command mode disabled");
  }
}

bool SCAN_IsCommandMode(void) { return scan.cmdCtx != NULL; }

void SCAN_CommandForceNext(void) {
  if (!scan.cmdCtx)
    return;

  Log("[SCAN] Force next command");
  if (!SCMD_Advance(scan.cmdCtx)) {
    SCMD_Rewind(scan.cmdCtx);
  }

  scan.rangeActive = false;
  ChangeState(SCAN_STATE_IDLE);
  gRedrawScreen = true;
}

SCMD_Command *SCAN_GetCurrentCommand(void) {
  return scan.cmdCtx ? SCMD_GetCurrent(scan.cmdCtx) : NULL;
}

SCMD_Command *SCAN_GetNextCommand(void) {
  return scan.cmdCtx ? SCMD_GetNext(scan.cmdCtx) : NULL;
}

uint16_t SCAN_GetCommandIndex(void) {
  return scan.cmdCtx ? SCMD_GetCurrentIndex(scan.cmdCtx) : 0;
}

uint16_t SCAN_GetCommandCount(void) {
  return scan.cmdCtx ? SCMD_GetCommandCount(scan.cmdCtx) : 0;
}

uint16_t SCAN_GetSquelchLevel() { return scan.squelchLevel; }
