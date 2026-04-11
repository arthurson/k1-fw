#include "scan.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../helper/lootlist.h"
#include "../helper/scancommand.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/spectrum.h"
#include "bands.h"
#include "measurements.h"

#define GARBAGE_FREQ_STEP 650000U
#define STE_DEBOUNCE_MS 250 // окно подавления STE-хвоста

// --- Алгоритмы сканирования ---

const char *SCAN_ALGO_NAMES[] = {
    [SCAN_ALGO_ADAPTIVE] = "Adaptive",
    [SCAN_ALGO_FULLRESET] = "FullReset",
    [SCAN_ALGO_STATISTICAL] = "Statistical",
    [SCAN_ALGO_CALIBRATED] = "Calibrated",
};

static ScanAlgo scanAlgo = SCAN_ALGO_ADAPTIVE;

// --- Параметры статистического алгоритма ---
#define STAT_PASSES 3       // замеров на канал
#define STAT_WARMUP_US 2000 // dwell каждого замера

// --- Параметры калибровочного алгоритма ---
#define CAL_WARMUP_US 8000 // dwell при калибровке
#define CAL_PASSES 2       // проходов калибровки
#define MAX_CAL_CHANNELS                                                       \
  640 // макс. каналов (~400 при 12.5кГц на 8МГц диапазон)
#define CAL_NOISE_MARGIN 8 // превышение над калибровочным полом

static uint8_t calFloor[MAX_CAL_CHANNELS]; // откалиброванный шум по каналам
static uint16_t calChannelCount = 0;
static bool calDone = false;

// --- Адаптивный детектор (EMA) ---
#define ADAP_MIN_SAMPLES 8 // прогрев
#define ADAP_EMA_SHIFT 4   // alpha = 1/16
#define ADAP_WARMUP_SHIFT 2 // alpha = 1/4 при прогреве (быстрая сходимость)
#define DELTA_RSSI_THRESH 8  // порог резкого роста rssi
#define DELTA_NOISE_THRESH 4 // порог резкого падения noise
#define FLOOR_MARGIN_RSSI 6  // запас над EMA rssi
#define FLOOR_MARGIN_NOISE 3 // запас под EMA noise
#define FLOOR_MARGIN_GLITCH 3

typedef struct {
  uint16_t rssiEma; // значение << ADAP_EMA_SHIFT
  uint16_t noiseEma;
  uint16_t glitchEma;
  uint8_t count;
  uint8_t prevRssi;
  uint8_t prevNoise;
  uint8_t prevGlitch;
} AdaptiveFloor;

static AdaptiveFloor afloor;

static ScanContext scan = {
    .state = SCAN_STATE_IDLE,
    .mode = SCAN_MODE_SINGLE,
    .warmupUs = 2500,
    .checkDelayMs = SQL_DELAY,
    .isOpen = false,
    .cmdRangeActive = false,
    .cmdCtx = NULL,
};

static SCMD_Context cmdctx;
static uint32_t sqReopenAt = 0;

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

static void STE_StartGate(void) {
  if (ctx->code.type != 0)
    sqReopenAt = Now() + STE_DEBOUNCE_MS;
}

static bool IsSqOpenGated(void) {
  return BK4819_IsSquelchOpen() && (Now() >= sqReopenAt);
}

// --- Адаптивный пол шума (EMA) ---

// обновление EMA: ema += (sample - ema) >> shift
static inline uint16_t EmaUpdate(uint16_t ema, uint8_t sample, uint8_t shift) {
  int16_t delta = (int16_t)(((uint16_t)sample << ADAP_EMA_SHIFT) - ema);
  return ema + (delta >> shift);
}

static inline uint8_t EmaGet(uint16_t ema) {
  return (uint8_t)(ema >> ADAP_EMA_SHIFT);
}

static void AdapFloor_Reset(void) {
  afloor.count = 0;
  afloor.prevRssi = 0;
  afloor.prevNoise = 255;
  afloor.prevGlitch = 255;
  afloor.rssiEma = 0;
  afloor.noiseEma = 0;
  afloor.glitchEma = 0;
}

static void AdapFloor_UpdateEma(uint8_t rssi, uint8_t noise, uint8_t glitch) {
  if (afloor.count == 0) {
    // первый сэмпл — инициализация
    afloor.rssiEma = (uint16_t)rssi << ADAP_EMA_SHIFT;
    afloor.noiseEma = (uint16_t)noise << ADAP_EMA_SHIFT;
    afloor.glitchEma = (uint16_t)glitch << ADAP_EMA_SHIFT;
  } else {
    // при прогреве быстрее, потом медленнее
    uint8_t sh =
        (afloor.count < ADAP_MIN_SAMPLES) ? ADAP_WARMUP_SHIFT : ADAP_EMA_SHIFT;
    afloor.rssiEma = EmaUpdate(afloor.rssiEma, rssi, sh);
    afloor.noiseEma = EmaUpdate(afloor.noiseEma, noise, sh);
    afloor.glitchEma = EmaUpdate(afloor.glitchEma, glitch, sh);
  }
  if (afloor.count < 255)
    afloor.count++;
}

static bool AdaptiveSq_Check(uint8_t rssi, uint8_t noise, uint8_t glitch) {
  // прогрев: набираем фон, пропускаем детекцию
  if (afloor.count < ADAP_MIN_SAMPLES) {
    AdapFloor_UpdateEma(rssi, noise, glitch);
    afloor.prevRssi = rssi;
    afloor.prevNoise = noise;
    afloor.prevGlitch = glitch;
    return false;
  }

  uint8_t floorR = EmaGet(afloor.rssiEma);
  uint8_t floorN = EmaGet(afloor.noiseEma);
  uint8_t floorG = EmaGet(afloor.glitchEma);

  // 1) выше адаптивного пола?
  // Базовый порог снижен до 4 (было 6), но подтверждение шума/глитча
  // обязательно
  bool rssi_above = (rssi > floorR + FLOOR_MARGIN_RSSI - 2);
  bool noise_ok = (noise + FLOOR_MARGIN_NOISE < floorN ||
                   glitch + FLOOR_MARGIN_GLITCH < floorG);
  bool above_floor = rssi_above && noise_ok;

  // 2) резкий фронт относительно предыдущего шага?
  int16_t dR = (int16_t)rssi - afloor.prevRssi;
  int16_t dN = (int16_t)noise - afloor.prevNoise;
  bool sharp_front = (dR > DELTA_RSSI_THRESH) && (dN < -DELTA_NOISE_THRESH);

  afloor.prevRssi = rssi;
  afloor.prevNoise = noise;
  afloor.prevGlitch = glitch;

  bool candidate = above_floor || sharp_front;

  // EMA обновляем только фоном — кандидаты не загрязняют пол
  if (!candidate)
    AdapFloor_UpdateEma(rssi, noise, glitch);

  return candidate;
}

static bool IsSkippable(uint32_t f) {
  if (gSettings.skipGarbageFrequencies && (f % GARBAGE_FREQ_STEP == 0))
    return true;
  Loot *l = LOOT_Get(f);
  return l && (l->blacklist || l->whitelist);
}

static void ChangeState(ScanState s) {
  if (scan.state != s) {
    scan.state = s;
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
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, vfo->msm.f, false);
  RADIO_SetParam(ctx, PARAM_STEP, gCurrentBand.step, false);
  RADIO_ApplySettings(ctx);
  SP_Init(&gCurrentBand);
  if (gLastActiveLoot && !BANDS_InRange(gLastActiveLoot->f, &gCurrentBand))
    gLastActiveLoot = NULL;
}

// сброс только prev, чтобы дельта не сработала ложно на стыке диапазонов
// EMA сохраняем — адаптируется сама
static void AdapFloor_SoftReset(void) {
  afloor.prevRssi = 255;
  afloor.prevNoise = 0;
  afloor.prevGlitch = 0;
}

static void BeginScanRange(uint32_t start, uint32_t end, uint16_t step) {
  scan.startF = start;
  scan.endF = end;
  scan.currentF = start;
  scan.stepF = step;
  scan.cmdRangeActive = true;
  calDone = false; // сброс при смене диапазона
  AdapFloor_SoftReset();
  ChangeState(SCAN_STATE_TUNING);
}

// Блокирующая калибровка шумовой полки.
// Вызывается один раз при старте диапазона (algo == CALIBRATED).
// Усредняет noise по CAL_PASSES проходов → calFloor[].
static void RunCalibration(void) {
  if (scan.stepF == 0)
    return;

  uint32_t n = 0;
  for (uint32_t f = scan.startF; f <= scan.endF && n < MAX_CAL_CHANNELS;
       f += scan.stepF, n++)
    calFloor[n] = 0;
  calChannelCount = (uint16_t)n;

  for (uint8_t pass = 0; pass < CAL_PASSES; pass++) {
    uint16_t idx = 0;
    for (uint32_t f = scan.startF; f <= scan.endF && idx < calChannelCount;
         f += scan.stepF, idx++) {
      RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
      RADIO_SetParam(ctx, PARAM_FREQUENCY, f, false);
      RADIO_ApplySettings(ctx);
      SYSTICK_DelayUs(CAL_WARMUP_US);
      // усредняем noise (не rssi — ищем именно шумовую полку)
      calFloor[idx] += BK4819_GetNoise() / CAL_PASSES;
    }
  }
  calDone = true;
}

static void UpdateBandAndRestart(void) {
  ApplyBandSettings();
  if (scan.mode == SCAN_MODE_FREQUENCY || scan.mode == SCAN_MODE_ANALYSER)
    BeginScanRange(gCurrentBand.start, gCurrentBand.end,
                   StepFrequencyTable[gCurrentBand.step]);
}

// ============================================================================

static void ApplyCommand(SCMD_Command *cmd) {
  if (!cmd)
    return;

  switch (cmd->type) {
  case SCMD_CHANNEL:
    BeginScanRange(cmd->start, cmd->start, 0);
    return;
  case SCMD_RANGE:
    BeginScanRange(cmd->start, cmd->end, cmd->step);
    return;
  case SCMD_PAUSE:
    SYSTICK_DelayMs(cmd->dwell_ms); // TODO: неблокирующая задержка
    break;
  default:
    break;
  }
  // MARKER, PAUSE (fall-through), JUMP, прочие — просто переходим дальше
  if (!SCMD_Advance(scan.cmdCtx))
    SCMD_Rewind(scan.cmdCtx);
}

static void HandleEndOfRange(void) {
  if (scan.cmdCtx) {
    if (!SCMD_Advance(scan.cmdCtx))
      SCMD_Rewind(scan.cmdCtx);
    scan.cmdRangeActive = false;
    ChangeState(SCAN_STATE_IDLE);
  } else {
    scan.currentF = scan.startF;
    AdapFloor_SoftReset();
    ChangeState(SCAN_STATE_TUNING);
    SP_Begin();
  }
  gRedrawScreen = true;
}

// ============================================================================

static void HandleStateIdle(void) {
  if (!scan.cmdCtx || scan.cmdRangeActive)
    return;
  SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
  if (cmd)
    ApplyCommand(cmd);
}

// Измерение на текущей частоте, заполняет scan.measurement
static void TakeMeasurement(bool precise) {
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, precise, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
  RADIO_ApplySettings(ctx);
  SYSTICK_DelayUs(scan.warmupUs);
  scan.measurement.rssi = RADIO_GetRSSI(ctx);
  scan.measurement.noise = BK4819_GetNoise();
  scan.measurement.glitch = BK4819_GetGlitch();
  scan.measurement.f = scan.currentF;
}

// ALGO_STATISTICAL: N замеров, берём максимум rssi
// Случайные glitch-пики усредняются, стабильный сигнал — нет
static void TakeMeasurementStatistical(bool precise) {
  uint8_t maxRssi = 0, minNoise = 255, minGlitch = 255;
  RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, precise, false);
  RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
  RADIO_ApplySettings(ctx);

  for (uint8_t i = 0; i < STAT_PASSES; i++) {
    SYSTICK_DelayUs(STAT_WARMUP_US);
    uint8_t r = RADIO_GetRSSI(ctx);
    uint8_t n = BK4819_GetNoise();
    uint8_t g = BK4819_GetGlitch();
    if (r > maxRssi)
      maxRssi = r;
    if (n < minNoise)
      minNoise = n;
    if (g < minGlitch)
      minGlitch = g;
  }
  scan.measurement.rssi = maxRssi;
  scan.measurement.noise = minNoise;
  scan.measurement.glitch = minGlitch;
  scan.measurement.f = scan.currentF;
}

// Детектор для ALGO_CALIBRATED: сигнал = rssi сильно выше калибровочного шума
static bool CalibratedSq_Check(uint8_t rssi, uint8_t noise, uint8_t glitch) {
  if (glitch > 200 || scan.stepF == 0)
    return false;
  uint16_t idx = (uint16_t)((scan.currentF - scan.startF) / scan.stepF);
  if (idx >= calChannelCount)
    return false;
  uint8_t floor = calFloor[idx];
  // сигнал: rssi хорошо выше полки И noise упал ниже полки
  return (rssi > floor + CAL_NOISE_MARGIN) &&
         (noise + (CAL_NOISE_MARGIN / 2) < floor || glitch < 50);
}

static void HandleStateTuning(void) {
  if (scan.stepF == 0) {
    if (IsSkippable(scan.currentF)) {
      HandleEndOfRange();
      return;
    }
  } else {
    while (scan.currentF <= scan.endF && IsSkippable(scan.currentF))
      scan.currentF += scan.stepF;
  }

  if (scan.currentF > scan.endF) {
    HandleEndOfRange();
    return;
  }

  // CALIBRATED: калибровка при первом проходе
  if (scanAlgo == SCAN_ALGO_CALIBRATED && !calDone)
    RunCalibration();

  RADIO_MuteAudioNow(gRadioState);

  bool precise =
      (scanAlgo == SCAN_ALGO_FULLRESET || scanAlgo == SCAN_ALGO_CALIBRATED);

  if (scanAlgo == SCAN_ALGO_STATISTICAL)
    TakeMeasurementStatistical(precise);
  else
    TakeMeasurement(precise);

  scan.scanCycles++;
  UpdateCPS();

  SP_AddPoint(&scan.measurement);
  if (scan.mode == SCAN_MODE_ANALYSER) {
    scan.currentF += scan.stepF;
    return;
  }

  bool isCandidate;
  switch (scanAlgo) {
  case SCAN_ALGO_CALIBRATED:
    isCandidate = CalibratedSq_Check(
        scan.measurement.rssi, scan.measurement.noise, scan.measurement.glitch);
    break;
  case SCAN_ALGO_STATISTICAL:
    // статистический: тот же EMA, но данные уже усреднены
    isCandidate = AdaptiveSq_Check(
        scan.measurement.rssi, scan.measurement.noise, scan.measurement.glitch);
    break;
  default: // ADAPTIVE, FULLRESET
    isCandidate = AdaptiveSq_Check(
        scan.measurement.rssi, scan.measurement.noise, scan.measurement.glitch);
    break;
  }

  if (isCandidate) {
    ChangeState(SCAN_STATE_CHECKING);
  } else {
    scan.measurement.open = false;
    scan.currentF += scan.stepF;
  }
}

static void HandleStateChecking(void) {
  static uint32_t lastEnteredAt = 0;
  static bool vcoResetDone = false;
  static uint32_t vcoResetAt = 0;

  if (scan.stateEnteredAt != lastEnteredAt) {
    lastEnteredAt = scan.stateEnteredAt;
    vcoResetDone = false;
    vcoResetAt = 0;
  }

  if (!vcoResetDone) {
    if (scanAlgo == SCAN_ALGO_FULLRESET || scanAlgo == SCAN_ALGO_CALIBRATED) {
      // полный сброс через precise: частота переприменяется с полной
      // инициализацией PLL
      RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
      RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
      RADIO_ApplySettings(ctx);
    } else {
      // VCO-only reset
      RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, false, false);
      RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
      RADIO_ApplySettings(ctx);
    }

    vcoResetAt = Now(); // отсчёт SQL_DELAY с этого момента
    vcoResetDone = true;
    return;
  }

  if (Now() - vcoResetAt < scan.checkDelayMs)
    return;

  bool isOpen = BK4819_IsSquelchOpen();

  scan.isOpen = isOpen;
  scan.measurement.open = isOpen;
  scan.measurement.f = scan.currentF;
  LOOT_Update(&scan.measurement);

  if (isOpen) {
    vfo->is_open = true;
    RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
    gRedrawScreen = true;

    if (scan.cmdCtx) {
      SCMD_Command *cmd = SCMD_GetCurrent(scan.cmdCtx);
      if (cmd && (cmd->flags & SCMD_FLAG_AUTO_WHITELIST))
        LOOT_WhitelistLast();
    }

    ChangeState(SCAN_STATE_LISTENING);
  } else {
    // выравнивание PLL: прогрев через предыдущую частоту и обратно
    if (scan.stepF > 0) {
      RADIO_SetParam(ctx, PARAM_PRECISE_F_CHANGE, true, false);
      RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF - scan.stepF, false);
      RADIO_ApplySettings(ctx);
      SYSTICK_DelayUs(scan.warmupUs);

      RADIO_SetParam(ctx, PARAM_FREQUENCY, scan.currentF, false);
      RADIO_ApplySettings(ctx);
      SYSTICK_DelayUs(scan.warmupUs);
    }

    // EMA обновляем только если rssi строго ниже порога детекции —
    // иначе сигнал-кандидат поднимает пол вверх
    uint8_t floorR = EmaGet(afloor.rssiEma);
    if (scan.measurement.rssi < floorR + FLOOR_MARGIN_RSSI)
      AdapFloor_UpdateEma(scan.measurement.rssi, scan.measurement.noise,
                          scan.measurement.glitch);

    scan.currentF += scan.stepF;
    ChangeState(SCAN_STATE_TUNING);
  }
}

static uint32_t sqClosedAt = 0;

static void HandleStateListening(void) {
  if (Now() - scan.radioTimer < SQL_DELAY)
    return;
  scan.radioTimer = Now();

  bool wasOpen = scan.isOpen;
  scan.isOpen = IsSqOpenGated();

  if (scan.isOpen != wasOpen) {
    if (wasOpen) {
      STE_StartGate();
      RADIO_MuteAudioNow(gRadioState);
      sqClosedAt = Now();
    } else {
      vfo->is_open = true;
      RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
      sqClosedAt = 0;
    }
    gRedrawScreen = true;
  }

  bool shouldLeave;
  if (scan.isOpen) {
    // открыт: уходим по общему таймауту пребывания
    shouldLeave = ElapsedMs() >= SCAN_TIMEOUTS[gSettings.sqOpenedTimeout];
  } else {
    // закрыт: уходим по времени с момента закрытия
    shouldLeave = sqClosedAt && (Now() - sqClosedAt >=
                                 SCAN_TIMEOUTS[gSettings.sqClosedTimeout]);
  }

  if (shouldLeave) {
    RADIO_MuteAudioNow(gRadioState);
    scan.currentF += scan.stepF;
    sqClosedAt = 0;
    ChangeState(SCAN_STATE_TUNING);
    gRedrawScreen = true;
  }
}

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

void SCAN_Check(void) {
  RADIO_CheckAndSaveVFO(gRadioState);
  if (scan.mode == SCAN_MODE_NONE)
    return;

  // мультивотч только в SINGLE — при активном сканировании он конфликтует с
  // радио
  if (scan.mode == SCAN_MODE_SINGLE)
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

void SCAN_SetMode(ScanMode mode) {
  if (scan.cmdCtx && mode != scan.mode)
    SCAN_SetCommandMode(false);

  scan.mode = mode;
  scan.scanCycles = 0;
  ChangeState(SCAN_STATE_IDLE);

  switch (mode) {
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
  default:
    break;
  }
}

ScanMode SCAN_GetMode(void) { return scan.mode; }

void SCAN_Init(void) {
  scan.lastCpsTime = Now();
  scan.scanCycles = 0;
  scan.currentCps = 0;
  scan.radioTimer = Now();
  AdapFloor_Reset();

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
  RADIO_MuteAudioNow(gRadioState);
  scan.currentF += scan.stepF;
  RADIO_SwitchAudioToVFO(gRadioState, gRadioState->active_vfo_index);
  ChangeState(SCAN_STATE_TUNING);
}

void SCAN_NextBlacklist(void) {
  LOOT_BlacklistLast();
  gRedrawScreen = true;
  SCAN_Next();
}

void SCAN_NextWhitelist(void) {
  LOOT_WhitelistLast();
  gRedrawScreen = true;
  SCAN_Next();
}

void SCAN_SetDelay(uint32_t delay) { scan.warmupUs = delay; }
uint32_t SCAN_GetDelay(void) { return scan.warmupUs; }
uint32_t SCAN_GetCps(void) { return scan.currentCps; }

// ============================================================================

void SCAN_LoadCommandFile(const char *filename) {
  if (scan.cmdCtx)
    SCAN_SetCommandMode(false);

  scan.cmdCtx = &cmdctx;

  if (SCMD_Init(scan.cmdCtx, filename)) {
    scan.mode = SCAN_MODE_FREQUENCY;
    scan.cmdRangeActive = false;
    ChangeState(SCAN_STATE_IDLE);
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
  }
}

bool SCAN_IsCommandMode(void) { return scan.cmdCtx != NULL; }

void SCAN_CommandForceNext(void) {
  if (!scan.cmdCtx)
    return;
  if (!SCMD_Advance(scan.cmdCtx))
    SCMD_Rewind(scan.cmdCtx);
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

// ============================================================================

void SCAN_HandleInterrupt(uint16_t int_bits) {
  const uint16_t tail_mask = BK4819_REG_02_MASK_CxCSS_TAIL |
                             BK4819_REG_02_MASK_CTCSS_LOST |
                             BK4819_REG_02_MASK_CDCSS_LOST;
  if (int_bits & tail_mask) {
    STE_StartGate();
    RADIO_MuteAudioNow(gRadioState);
    scan.isOpen = false;
    gRedrawScreen = true;
  }
}

bool SCAN_IsSqOpen(void) { return BK4819_IsSquelchOpen(); }
const char *SCAN_GetStateName(void) { return SCAN_STATE_NAMES[scan.state]; }
ScanState SCAN_GetState(void) { return scan.state; }

// --- Выбор алгоритма сканирования ---

void SCAN_SetAlgo(ScanAlgo algo) {
  if (algo >= SCAN_ALGO_COUNT)
    return;
  scanAlgo = algo;
  calDone = false; // при смене алго сбрасываем калибровку
  AdapFloor_Reset();
  Log("[SCAN] algo: %s", SCAN_ALGO_NAMES[algo]);
}

ScanAlgo SCAN_GetAlgo(void) { return scanAlgo; }
const char *SCAN_GetAlgoName(void) { return SCAN_ALGO_NAMES[scanAlgo]; }
