#include "scan.h"
#include "../apps/apps.h"
#include "../dcs.h"
#include "../driver/st7565.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/lootlist.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/spectrum.h"
#include "../ui/toast.h"
#include "bands.h"
#include "measurements.h"
#include <stdbool.h>

// ============================================================================
// Глобальный контекст
// ============================================================================

static ScanContext scan = {
    .state = SCAN_STATE_IDLE,
    .mode = SCAN_MODE_SINGLE,
    .warmupUs = 1200,
    .checkDelayMs = SQL_DELAY,
    .squelchLevel = 0,
    .isOpen = false,
    .cmdRangeActive = false,
    .cmdCtx = NULL,
};

// Последнее состояние squelch из прерывания (ISR → main loop)
static bool sqOpen = false;

static SCMD_Context cmdctx;

const char *SCAN_MODE_NAMES[] = {
    [SCAN_MODE_NONE] = "None",         [SCAN_MODE_SINGLE] = "VFO",
    [SCAN_MODE_FREQUENCY] = "Scan",    [SCAN_MODE_CHANNEL] = "CH Scan",
    [SCAN_MODE_ANALYSER] = "Analyser", [SCAN_MODE_MULTIWATCH] = "MultiWatch",
};

const char *SCAN_STATE_NAMES[] = {
    [SCAN_STATE_IDLE] = "Idle",
    [SCAN_STATE_TUNING] = "Tuning",
    [SCAN_STATE_CHECKING] = "Checking",
    [SCAN_STATE_LISTENING] = "Listening",
};

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void ChangeState(ScanState newState) {
  if (scan.state != newState) {
    scan.state = newState;
    scan.stateEnteredAt = Now();
  }
}

static uint32_t ElapsedMs(void) { return Now() - scan.stateEnteredAt; }

static void UpdateCPS(void) {
  uint32_t now = Now();
  uint32_t elapsed = now - scan.lastCpsTime;
  if (elapsed >= 1000) {
    scan.currentCps = (scan.scanCycles * 1000) / elapsed;
    scan.lastCpsTime = now;
    scan.scanCycles = 0;
  }
}

static void ApplyBandSettings(void) {
  vfo->msm.f = gCurrentBand.start;
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
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

static void BeginScanRange(uint32_t start, uint32_t end, uint16_t step) {
  scan.startF = start;
  scan.endF = end;
  scan.currentF = start;
  scan.stepF = step;
  scan.cmdRangeActive = true;

  Log("[SCAN] Range: %u-%u Hz, step=%u", start, end, step);
  ChangeState(SCAN_STATE_TUNING);
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
    BeginScanRange(cmd->start, cmd->start, 0);
    break;

  case SCMD_RANGE:
    BeginScanRange(cmd->start, cmd->end, cmd->step);
    break;

  case SCMD_PAUSE:
    Log("[SCAN] Pause %u ms", cmd->dwell_ms);
    SYSTICK_DelayMs(cmd->dwell_ms);
    /* fall through */
  case SCMD_MARKER:
  default:
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
    }
    break;

  case SCMD_JUMP:
    Log("[SCAN] Jump — not implemented");
    break;
  }
}

// Вызывается когда currentF вышел за endF
static void HandleEndOfRange(void) {
  if (scan.cmdCtx) {
    // Командный режим: переходим к следующей команде
    if (!SCMD_Advance(scan.cmdCtx)) {
      SCMD_Rewind(scan.cmdCtx);
      Log("[SCAN] Command sequence restarted");
    }
    scan.cmdRangeActive = false;
    ChangeState(SCAN_STATE_IDLE);
  } else {
    // Обычный режим: возврат к началу диапазона
    scan.currentF = scan.startF;
    ChangeState(SCAN_STATE_TUNING);
    SP_Begin();
  }
  gRedrawScreen = true;
}

// ============================================================================
// State machine — частотное сканирование
// ============================================================================

static void HandleStateIdle(void) {
  // Ожидаем следующую команду в командном режиме
  if (scan.cmdCtx && !scan.cmdRangeActive) {
    SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
    if (cmd)
      ApplyCommand(cmd);
  }
}

static void HandleStateTuning(void) {
  if (scan.currentF > scan.endF) {
    HandleEndOfRange();
    return;
  }

  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
  RADIO_ApplySettings(ctx);

  SYSTICK_DelayUs(scan.warmupUs);
  scan.measurement.rssi = RADIO_GetRSSI(ctx);
  scan.measurement.f = scan.currentF;

  scan.scanCycles++;
  scan.idleCycles++;
  // Каждые 32 цикла без открытого squelch — снижаем порог
  if (scan.idleCycles >= 32) {
    if (scan.squelchLevel)
      scan.squelchLevel--;
    scan.idleCycles = 0;
  }
  UpdateCPS();

  // Анализатор: просто пишем точку и едем дальше, без squelch-проверки
  if (scan.mode == SCAN_MODE_ANALYSER) {
    SP_AddPoint(&scan.measurement);
    scan.currentF += scan.stepF;
    // состояние не меняем — остаёмся в TUNING
    return;
  }

  // Инициализируем программный порог по первому измерению
  if (!scan.squelchLevel && scan.measurement.rssi) {
    scan.squelchLevel = scan.measurement.rssi - 1;
  }

  bool softOpen = scan.measurement.rssi >= scan.squelchLevel;
  // Пропускаем «мусорные» частоты
  if (softOpen && gSettings.skipGarbageFrequencies &&
      scan.currentF % 650000 == 0) {
    softOpen = false;
  }

  if (softOpen) {
    // Возможен сигнал — ждём аппаратного подтверждения
    ChangeState(SCAN_STATE_CHECKING);
  } else {
    // Сигнала нет — на следующую частоту
    scan.measurement.open = false;
    SP_AddPoint(&scan.measurement);
    scan.currentF += scan.stepF;
    // остаёмся в TUNING
  }
}

static void HandleStateChecking(void) {
  if (ElapsedMs() < scan.checkDelayMs)
    return;

  RADIO_UpdateSquelch(gRadioState);

  scan.isOpen = vfo->is_open;
  scan.measurement.open = scan.isOpen;
  LOOT_Update(&scan.measurement);
  SP_AddPoint(&scan.measurement);

  if (scan.isOpen) {
    // Сигнал подтверждён
    gRedrawScreen = true;

    // Авто-вайтлист из командного файла
    if (scan.cmdCtx) {
      SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
      if (cmd && (cmd->flags & SCMD_FLAG_AUTO_WHITELIST)) {
        LOOT_WhitelistLast();
        Log("[SCAN] Auto-whitelisted %u Hz", scan.currentF);
      }
    }

    scan.idleCycles = 0; // сигнал найден — сбрасываем счётчик
    ChangeState(SCAN_STATE_LISTENING);
  } else {
    // Ложное срабатывание программного фильтра — повышаем порог и идём дальше
    scan.squelchLevel++;
    scan.currentF +=
        scan.stepF; // сдвигаем здесь, чтобы не перепроверять ту же частоту
    ChangeState(SCAN_STATE_TUNING);
  }
}

static void HandleStateListening(void) {
  bool wasOpen = scan.isOpen;
  scan.isOpen = sqOpen; // состояние из прерывания

  SP_AddPoint(&scan.measurement);
  RADIO_UpdateSquelch(gRadioState);

  if (wasOpen != scan.isOpen) {
    gRedrawScreen = true;
  }

  uint32_t timeout = scan.isOpen ? SCAN_TIMEOUTS[gSettings.sqOpenedTimeout]
                                 : SCAN_TIMEOUTS[gSettings.sqClosedTimeout];

  if (ElapsedMs() >= timeout) {
    Log("[SCAN] %s timeout", scan.isOpen ? "Listen" : "Close");
    ChangeState(SCAN_STATE_TUNING);
    gRedrawScreen = true;
  }
}

// ============================================================================
// State machine — Single VFO (мониторинг)
// ============================================================================

static uint32_t radioTimer = 0;
static void HandleModeSingle(void) {
  scan.measurement.rssi = vfo->msm.rssi;
  scan.measurement.noise = vfo->msm.noise;
  scan.measurement.glitch = vfo->msm.glitch;
  scan.measurement.snr = vfo->msm.snr;

  if (Now() - radioTimer >= SQL_DELAY) {
    RADIO_UpdateSquelch(gRadioState);
    SP_ShiftGraph(-1);
    SP_AddGraphPoint(&scan.measurement);
    radioTimer = Now();
  }
}

// ============================================================================
// Главная функция
// ============================================================================

void SCAN_Check(void) {
  RADIO_CheckAndSaveVFO(gRadioState);
  if (scan.mode == SCAN_MODE_NONE)
    return;

  RADIO_UpdateMultiwatch(gRadioState);

  if (scan.mode == SCAN_MODE_SINGLE) {
    HandleModeSingle();
    return;
  }

  switch (scan.state) {
  case SCAN_STATE_IDLE:
    HandleStateIdle();
    break;
  case SCAN_STATE_TUNING:
    HandleStateTuning();
    break;
  case SCAN_STATE_CHECKING:
    HandleStateChecking();
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
    scan.cmdRangeActive = false;
    break;
  case SCAN_MODE_FREQUENCY:
  case SCAN_MODE_ANALYSER:
    ApplyBandSettings();
    BeginScanRange(gCurrentBand.start, gCurrentBand.end,
                   StepFrequencyTable[gCurrentBand.step]);
    break;
  case SCAN_MODE_CHANNEL:
    // TODO: channel mode
    break;
  case SCAN_MODE_MULTIWATCH:
    // Обрабатывается в RADIO_UpdateMultiwatch
    break;
  }
}

ScanMode SCAN_GetMode(void) { return scan.mode; }

void SCAN_Init(bool multiband) {
  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;

  ApplyBandSettings();
  vfo->is_open = false;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
}

void SCAN_setBand(Band b) {
  gCurrentBand = b;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    BeginScanRange(gCurrentBand.start, gCurrentBand.end,
                   StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setStartF(uint32_t f) {
  gCurrentBand.start = f;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    BeginScanRange(f, gCurrentBand.end, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setEndF(uint32_t f) {
  gCurrentBand.end = f;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    BeginScanRange(gCurrentBand.start, f,
                   StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_setRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.start = fs;
  gCurrentBand.end = fe;
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    BeginScanRange(fs, fe, StepFrequencyTable[gCurrentBand.step]);
  }
}

void SCAN_Next(void) {
  vfo->is_open = false;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  ChangeState(SCAN_STATE_TUNING);
}

void SCAN_NextBlacklist(void) {
  LOOT_BlacklistLast();
  SCAN_Next();
}
void SCAN_NextWhitelist(void) {
  LOOT_WhitelistLast();
  SCAN_Next();
}

void SCAN_SetDelay(uint32_t delay) { scan.warmupUs = delay; }
uint32_t SCAN_GetDelay(void) { return scan.warmupUs; }
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
    scan.cmdRangeActive = false;
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
    scan.cmdRangeActive = false;
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
  scan.cmdRangeActive = false;
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
uint16_t SCAN_GetSquelchLevel(void) { return scan.squelchLevel; }

// ============================================================================
// Обработка прерываний BK4819
// ============================================================================

void SCAN_HandleInterrupt(uint16_t int_bits) {
  if (ctx->code.type == 0 && (int_bits & BK4819_REG_02_MASK_SQUELCH_LOST)) {
    Log("[SCAN] SQ lost (open)");
    sqOpen = true;
    RADIO_UnmuteAudioNow(gRadioState);
    gRedrawScreen = true;

    // Прерывание застало нас на переключении — форсируем LISTENING
    if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_CHANNEL) {
      if (scan.state == SCAN_STATE_TUNING ||
          scan.state == SCAN_STATE_CHECKING) {
        scan.measurement.f = scan.currentF;
        scan.measurement.open = true;
        LOOT_Update(&scan.measurement);
        ChangeState(SCAN_STATE_LISTENING);
      }
    }
  }

  if (int_bits & BK4819_REG_02_MASK_SQUELCH_FOUND) {
    Log("[SCAN] SQ found (closed)");
    sqOpen = false;
    RADIO_MuteAudioNow(gRadioState);
    gRedrawScreen = true;
  }

  // CSS/CTCSS/CDCSS — закрываем squelch
  if ((int_bits & BK4819_REG_02_MASK_CxCSS_TAIL) ||
      (int_bits & BK4819_REG_02_MASK_CTCSS_FOUND) ||
      (int_bits & BK4819_REG_02_MASK_CDCSS_FOUND)) {
    sqOpen = false;
    RADIO_MuteAudioNow(gRadioState);
  }

  // CSS/CTCSS/CDCSS потеряны — открываем squelch
  if ((int_bits & BK4819_REG_02_MASK_CTCSS_LOST) ||
      (int_bits & BK4819_REG_02_MASK_CDCSS_LOST)) {
    sqOpen = true;
    RADIO_UnmuteAudioNow(gRadioState);
  }
}

bool SCAN_IsSqOpen(void) { return sqOpen; }
const char *SCAN_GetStateName(void) { return SCAN_STATE_NAMES[scan.state]; }
