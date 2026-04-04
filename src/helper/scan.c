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
// Константы
// ============================================================================

// Шаг "мусорных" частот, которые нужно пропускать
#define GARBAGE_FREQ_STEP 650000U

// Количество циклов до снижения порога сканирования
#define SQUELCH_IDLE_DECAY_CYCLES 32U

// ============================================================================
// Глобальный контекст
// ============================================================================

static ScanContext scan = {
    .state = SCAN_STATE_IDLE,
    .mode = SCAN_MODE_SINGLE,
    .warmupUs = 2500,
    .checkDelayMs = SQL_DELAY,
    .squelchLevel = 0,
    .isOpen = false,
    .cmdRangeActive = false,
    .cmdCtx = NULL,
};

// Флаг squelch из прерывания (ISR → main loop).
// volatile гарантирует, что компилятор не закэширует значение между
// обработчиком и основным циклом, даже при программных прерываниях.
static volatile bool sqOpen = false;

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

// Целочисленный квадратный корень (метод Ньютона)
static uint32_t isqrt32(uint32_t x) {
  if (x == 0)
    return 0;
  uint32_t root = x;
  while (1) {
    uint32_t next = (root + x / root) >> 1;
    if (next >= root)
      return root;
    root = next;
  }
}

static void NoiseHistory_Add(uint8_t rssi) {
  // Если буфер полон, вычитаем вытесняемое значение из сумм
  if (scan.noiseHist.count == NOISE_HISTORY_SIZE) {
    uint8_t old = scan.noiseHist.values[scan.noiseHist.idx];
    scan.noiseHist.sum -= old;
    scan.noiseHist.sum_sq -= (uint32_t)old * old;
  } else {
    scan.noiseHist.count++;
  }
  scan.noiseHist.values[scan.noiseHist.idx] = rssi;
  scan.noiseHist.sum += rssi;
  scan.noiseHist.sum_sq += (uint32_t)rssi * rssi;
  scan.noiseHist.idx = (scan.noiseHist.idx + 1) % NOISE_HISTORY_SIZE;

  // Пересчитываем среднее и stddev
  if (scan.noiseHist.count > 0) {
    scan.noiseHist.mean = scan.noiseHist.sum / scan.noiseHist.count;
    uint32_t mean32 = scan.noiseHist.mean;
    uint32_t variance = (scan.noiseHist.sum_sq / scan.noiseHist.count);
    if (variance >= mean32 * mean32)
      variance -= mean32 * mean32;
    else
      variance = 0;
    // Используем целочисленный sqrt (можно приблизить)
    scan.noiseHist.stddev = (uint8_t)isqrt32(variance);
  }
}

static void NoiseHistory_Clear(void) {
  scan.noiseHist.idx = 0;
  scan.noiseHist.count = 0;
  scan.noiseHist.sum = 0;
  scan.noiseHist.sum_sq = 0;
  scan.noiseHist.mean = 0;
  scan.noiseHist.stddev = 0;
  memset(scan.noiseHist.values, 0, sizeof(scan.noiseHist.values));
}

static uint8_t GetAdaptiveThreshold(void) {
  if (scan.noiseHist.count < 10) {
    // Недостаточно данных — используем статический порог (например, из
    // настроек) Можно также вернуть scan.squelchLevel, если он уже задан
    return scan.squelchLevel ? scan.squelchLevel : 10;
  }
  uint8_t thr = scan.noiseHist.mean + scan.noiseHist.k * scan.noiseHist.stddev;
  if (thr < 5)
    thr = 5; // минимальный порог
  if (thr > 250)
    thr = 250;
  return thr;
}

static bool IsGarbageFreq(uint32_t f) {
  return gSettings.skipGarbageFrequencies && (f % GARBAGE_FREQ_STEP == 0);
}

// Возвращает true если частоту нужно пропустить при сканировании:
// - "мусорная" частота
// - частота в чёрном или белом списке (уже обработана)
static bool IsSkippable(uint32_t f) {
  if (IsGarbageFreq(f))
    return true;
  Loot *l = LOOT_Get(f);
  return l && (l->blacklist || l->whitelist);
}

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

// Применяет параметры текущего диапазона к радио и инициализирует спектр.
static void ApplyBandSettings(void) {
  vfo->msm.f = gCurrentBand.start;
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
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

// Запускает сканирование в диапазоне [start, end] с шагом step.
// step == 0 допустим только для одноканального режима (SCMD_CHANNEL),
// где startF == endF и цикл перебора не используется.
static void BeginScanRange(uint32_t start, uint32_t end, uint16_t step) {
  scan.startF = start;
  scan.endF = end;
  scan.currentF = start;
  scan.stepF = step;
  scan.cmdRangeActive = true;

  NoiseHistory_Clear();
  scan.adaptiveThreshold = 0; // будет вычислен заново

  Log("[SCAN] Range: %u-%u Hz, step=%u", start, end, step);
  ChangeState(SCAN_STATE_TUNING);
}

// Вспомогательная функция для API-функций установки диапазона.
// Обновляет поля band, применяет настройки и перезапускает сканирование
// если активен режим сканирования по частоте/анализатор.
static void UpdateBandAndRestart(void) {
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER) {
    BeginScanRange(gCurrentBand.start, gCurrentBand.end,
                   StepFrequencyTable[gCurrentBand.step]);
  }
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
    // step=0 — канальный режим, currentF == endF, цикл перебора не нужен
    BeginScanRange(cmd->start, cmd->start, 0);
    break;

  case SCMD_RANGE:
    BeginScanRange(cmd->start, cmd->end, cmd->step);
    break;

  case SCMD_PAUSE:
    Log("[SCAN] Pause %u ms", cmd->dwell_ms);
    // TODO: заменить на неблокирующую задержку (таймер в ScanContext),
    //       чтобы не зависать здесь при длинных паузах
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

// Вызывается когда currentF вышел за endF.
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
    NoiseHistory_Clear();
    scan.adaptiveThreshold = 0;
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
  scan.adaptiveThreshold = GetAdaptiveThreshold();
  // Пропускаем все нежелательные частоты.
  // Защита от stepF == 0: в канальном режиме startF == endF,
  // поэтому если текущая частота скиппабельна — выходим сразу в EndOfRange.
  if (scan.stepF == 0) {
    if (IsSkippable(scan.currentF)) {
      HandleEndOfRange();
      return;
    }
  } else {
    while (scan.currentF <= scan.endF && IsSkippable(scan.currentF)) {
      scan.currentF += scan.stepF;
    }
  }

  if (scan.currentF > scan.endF) {
    HandleEndOfRange();
    return;
  }

  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
  RADIO_ApplySettings(ctx);

  SYSTICK_DelayUs(scan.warmupUs);
  scan.measurement.rssi = RADIO_GetRSSI(ctx);
  scan.measurement.f = scan.currentF;

  scan.scanCycles++;
  scan.idleCycles++;
  if (scan.idleCycles >= SQUELCH_IDLE_DECAY_CYCLES) {
    if (scan.squelchLevel)
      scan.squelchLevel--;
    scan.idleCycles = 0;
  }
  UpdateCPS();

  // Анализатор: просто пишем точку и едем дальше, без squelch-проверки
  if (scan.mode == SCAN_MODE_ANALYSER) {
    SP_AddPoint(&scan.measurement);
    scan.currentF += scan.stepF;
    return;
  }

  // Инициализируем программный порог по первому измерению
  if (!scan.squelchLevel && scan.measurement.rssi) {
    scan.squelchLevel = scan.measurement.rssi - 1;
  }

  bool softOpen = (scan.measurement.rssi >= scan.adaptiveThreshold);
  if (softOpen) {
    // возможен сигнал
    ChangeState(SCAN_STATE_CHECKING);
  } else {
    // сигнала нет — запоминаем уровень шума
    NoiseHistory_Add(scan.measurement.rssi);
    scan.measurement.open = false;
    SP_AddPoint(&scan.measurement);
    scan.currentF += scan.stepF;
  }
}

static void HandleStateChecking(void) {
  if (ElapsedMs() < scan.checkDelayMs)
    return;

  RADIO_UpdateSquelch(gRadioState);

  scan.isOpen = vfo->is_open;
  scan.measurement.open = scan.isOpen;
  scan.measurement.f = scan.currentF;
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

    scan.idleCycles = 0;
    ChangeState(SCAN_STATE_LISTENING);
  } else {
    // Ложное срабатывание — повышаем порог и переходим дальше
    NoiseHistory_Add(scan.measurement.rssi);
    scan.squelchLevel++;
    scan.currentF += scan.stepF;
    ChangeState(SCAN_STATE_TUNING);
  }
}

static void HandleStateListening(void) {
  bool wasOpen = scan.isOpen;
  scan.isOpen = sqOpen; // состояние из прерывания

  // закрываемся без учёта прерывания, вдруг залипло
  if (ctx->radio_type == RADIO_BK4819) {
    scan.isOpen = BK4819_IsSquelchOpen();
  }

  SP_AddPoint(&scan.measurement);
  RADIO_UpdateSquelch(gRadioState);

  if (wasOpen != scan.isOpen) {
    gRedrawScreen = true;
  }

  uint32_t timeout = scan.isOpen ? SCAN_TIMEOUTS[gSettings.sqOpenedTimeout]
                                 : SCAN_TIMEOUTS[gSettings.sqClosedTimeout];

  if (ElapsedMs() >= timeout) {
    Log("[SCAN] %s timeout", scan.isOpen ? "Listen" : "Close");
    scan.currentF += scan.stepF;
    ChangeState(SCAN_STATE_TUNING);
    gRedrawScreen = true;
  }
}

// ============================================================================
// State machine — Single VFO (мониторинг)
// ============================================================================

static void HandleModeSingle(void) {
  scan.measurement.rssi = vfo->msm.rssi;
  scan.measurement.noise = vfo->msm.noise;
  scan.measurement.glitch = vfo->msm.glitch;
  scan.measurement.snr = vfo->msm.snr;

  if (Now() - scan.radioTimer >= SQL_DELAY) {
    RADIO_UpdateSquelch(gRadioState);
    SP_ShiftGraph(-1);
    SP_AddGraphPoint(&scan.measurement);
    scan.radioTimer = Now();
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

  // Полный сброс счётчиков при смене режима
  scan.scanCycles = 0;
  scan.squelchLevel = 0;
  scan.idleCycles = 0;
  ChangeState(SCAN_STATE_IDLE);

  switch (mode) {
  case SCAN_MODE_NONE:
    break;
  case SCAN_MODE_SINGLE:
    scan.cmdRangeActive = false;
    scan.currentF = ctx->frequency;
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

void SCAN_Init(void) {

  scan.noiseHist.idx = 0;
  scan.noiseHist.count = 0;
  scan.noiseHist.sum = 0;
  scan.noiseHist.sum_sq = 0;
  scan.noiseHist.k = 2; // можно сделать настраиваемым через gSettings
  scan.adaptiveThreshold = 0;

  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;
  scan.radioTimer = Now();

  ApplyBandSettings();
  vfo->is_open = false;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
}

void SCAN_SetBand(Band b) {
  gCurrentBand = b;
  UpdateBandAndRestart();
}

void SCAN_SetStartF(uint32_t f) {
  gCurrentBand.start = f;
  UpdateBandAndRestart();
}

void SCAN_SetEndF(uint32_t f) {
  gCurrentBand.end = f;
  UpdateBandAndRestart();
}

void SCAN_SetRange(uint32_t fs, uint32_t fe) {
  gCurrentBand.start = fs;
  gCurrentBand.end = fe;
  UpdateBandAndRestart();
}

void SCAN_Next(void) {
  vfo->is_open = false;
  scan.currentF += scan.stepF;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  ChangeState(SCAN_STATE_TUNING);
}

void SCAN_NextBlacklist(void) {
  LOOT_BlacklistLast();
  sqOpen = false;
  RADIO_MuteAudioNow(gRadioState);
  gRedrawScreen = true;
  SCAN_Next();
}

void SCAN_NextWhitelist(void) {
  LOOT_WhitelistLast();
  sqOpen = false;
  RADIO_MuteAudioNow(gRadioState);
  gRedrawScreen = true;
  SCAN_Next();
}

void SCAN_SetDelay(uint32_t delay) { scan.warmupUs = delay; }
uint32_t SCAN_GetDelay(void) { return scan.warmupUs; }
uint32_t SCAN_GetCps(void) { return scan.currentCps; }

// ============================================================================
// Командный режим
// ============================================================================

void SCAN_LoadCommandFile(const char *filename) {
  // Закрываем предыдущий контекст перед открытием нового
  if (scan.cmdCtx) {
    SCAN_SetCommandMode(false);
  }

  scan.cmdCtx = &cmdctx;
  memset(scan.cmdCtx, 0, sizeof(SCMD_Context));

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
  // Squelch открылся (сигнал появился)
  if (ctx->code.type == 0 && (int_bits & BK4819_REG_02_MASK_SQUELCH_LOST)) {
    gRedrawScreen = true;
    Log("[SCAN] SQ lost (open)");

    uint32_t checkF =
        (scan.mode == SCAN_MODE_SINGLE) ? ctx->frequency : scan.currentF;

    if (IsSkippable(checkF)) {
      sqOpen = false;
      return;
    }

    sqOpen = true;
    RADIO_UnmuteAudioNow(gRadioState);

    // Прерывание застало нас в процессе перестройки — форсируем LISTENING
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

  // Squelch закрылся (сигнал пропал)
  if (int_bits & BK4819_REG_02_MASK_SQUELCH_FOUND) {
    Log("[SCAN] SQ found (closed)");

    // защита от преждевременного закрытия шумодава
    if (ctx->radio_type == RADIO_BK4819) {
      SYSTICK_DelayMs(5);
      if (BK4819_IsSquelchOpen()) {
        return;
      }
    }
    sqOpen = false;
    RADIO_MuteAudioNow(gRadioState);
    gRedrawScreen = true;
  }

  // Тон CTCSS/CDCSS найден — закрываем squelch
  const uint16_t tone_found_mask = BK4819_REG_02_MASK_CxCSS_TAIL |
                                   BK4819_REG_02_MASK_CTCSS_FOUND |
                                   BK4819_REG_02_MASK_CDCSS_FOUND;
  if (int_bits & tone_found_mask) {
    Log("[SCAN] TONE found (closed)");
    sqOpen = false;
    RADIO_MuteAudioNow(gRadioState);
  }

  // Тон CTCSS/CDCSS потерян — открываем squelch
  const uint16_t tone_lost_mask =
      BK4819_REG_02_MASK_CTCSS_LOST | BK4819_REG_02_MASK_CDCSS_LOST;
  if (int_bits & tone_lost_mask) {
    Log("[SCAN] TONE lost (open)");
    sqOpen = true;
    RADIO_UnmuteAudioNow(gRadioState);
  }
}

bool SCAN_IsSqOpen(void) { return sqOpen; }
const char *SCAN_GetStateName(void) { return SCAN_STATE_NAMES[scan.state]; }
ScanState SCAN_GetState(void) { return scan.state; }
