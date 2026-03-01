/*
 * goertzel.h — Goertzel-based tone demodulator
 *
 * Архитектура конвейера:
 *
 *   ADC (uint16, 9600 Hz)
 *    │
 *    ▼  N сэмплов накапливаем → 1 решение
 *   [1] Goertzel bank  — мощность на 1-2 частотах (Q12 fixed-point)
 *    │  out: power[0], power[1]
 *    │
 *    ▼
 *   [2] Noise floor    — медленный трекер шумового пола
 *    │  out: floor, squelch_open
 *    │
 *    ▼
 *   [3] Bit decision   — OOK: power > thr | FSK: power[0] vs power[1]
 *    │  out: bit_val (bool)
 *    │
 *    ▼  (всё ниже работает на частоте Fs/N, не Fs)
 *   [4] Baud detect    — GCD гистограмма длин импульсов
 *    │  out: spb (samples-per-bit на частоте Fs/N)
 *    │
 *    ▼
 *   [5] Bit sampler    — большинством за period spb
 *    │  out: bit_ready, bit_val
 *    │
 *    ▼
 *   [6] Framer         — SOF / EOF, сборка байтов
 *    │
 *    ├─► on_start()
 *    └─► on_packet(data, nbytes)
 *
 * ──────────────────────────────────────────────────────────────────
 * Режимы и рекомендуемые параметры (Fs = 9600 Hz):
 *
 *  Протокол              Режим    mark     space   N    бод
 *  ──────────────────────────────────────────────────────────
 *  OOK с субнесущей 1кГц  OOK   1000 Hz    —      48   ≤200
 *  Bell 202 / APRS        FSK   1200 Hz  2200 Hz    8   1200
 *  Bell 103 (модемы)      FSK   1270 Hz  1070 Hz   32    300
 *  POCSAG / FLEX paging   FSK   пользователь выбирает
 *  DTMF (1 группа)        FSK   см. DTMF_FREQS ниже
 *
 * ──────────────────────────────────────────────────────────────────
 * Как работает Goertzel (кратко):
 *
 *  Стандартный DFT бин k для блока N:
 *    s[n] = x[n] + 2·cos(2π·k/N)·s[n-1] − s[n-2]
 *    |X[k]|² = s[N]² + s[N-1]² − 2·cos·s[N]·s[N-1]
 *
 *  Здесь k не обязан быть целым — Goertzel работает на любой частоте f:
 *    coef = 2·cos(2π·f/Fs)
 *
 *  Стоимость: 2 умножения + 2 сложения на сэмпл (vs FFT: N/2·log₂N).
 *  Для N=8: 16 умножений на бит — идеально для Cortex-M0.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GOERTZEL_FS       9600u
#define GOERTZEL_MAX_BITS 256u
#define GOERTZEL_HIST_N   128u

/* ═══════════════════════════════════════════════════════════════
 *  Режим демодулятора
 * ═══════════════════════════════════════════════════════════════ */
typedef enum {
  TONE_OOK = 0, /* 1 тон: есть/нет → 1/0                    */
  TONE_FSK,     /* 2 тона: power[mark] vs power[space] → 1/0 */
} ToneMode;

/* ═══════════════════════════════════════════════════════════════
 *  [1] Goertzel — один частотный бин, Q12 fixed-point
 *
 *  Вход: x (int32, рекомендуется ±128 — см. GOERTZEL_SCALE)
 *  Выход: power (uint32) — пропорционален |X(f)|²
 *
 *  Q12 коэффициент:  coef_q12 = round(2·cos(2π·f/Fs) · 4096)
 *  Диапазон coef_q12: [-8192 .. +8192]
 *
 *  Переполнение: при N≤96 и |x|≤128 состояния s1,s2 ≤ 12288,
 *  все промежуточные значения умещаются в int32 без int64.
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  int32_t s1, s2;       /* состояние фильтра                    */
  int32_t coef_q12;     /* 2·cos(2π·f/Fs) в Q12                 */
  uint32_t power;       /* |X(f)|² последнего блока             */
} Goertzel;

/* GOERTZEL_SCALE: сдвиг вправо для масштабирования входа.
 * ADC: 0..4095, DC ≈ 2048.
 * После вычитания DC: ±2048 → сдвиг на 4 → ±128.
 * Уменьши если сигнал слабый (но риск переполнения при N>96). */
#define GOERTZEL_SCALE 4

/* float используется только здесь (вызывается 1-2 раза, не в hot path) */
void goertzel_init(Goertzel *g, float freq_hz);
void goertzel_reset(Goertzel *g);

/* Подать один сэмпл (уже масштабированный: (ADC-DC)>>SCALE).
 * Вызывается из hot path — никакого float, только int32. */
static inline void goertzel_push(Goertzel *g, int32_t x) {
  int32_t s0 = x + ((g->s1 * g->coef_q12) >> 12) - g->s2;
  g->s2 = g->s1;
  g->s1 = s0;
}

/* Вычислить мощность и сбросить состояние.
 * Вызывать после N сэмплов (т.е. раз в block_n вызовов push). */
uint32_t goertzel_finish(Goertzel *g);

/* ═══════════════════════════════════════════════════════════════
 *  [2] Noise floor — адаптивный трекер шумового пола
 *
 *  Работает на мощностях Goertzel (uint32).
 *  Мгновенный спад к минимуму, медленный подъём.
 *
 *  Squelch открывается: power > floor << SNR_SHIFT  (2^SNR_SHIFT × floor)
 *    SNR_SHIFT=1 → 6 дБ; SNR_SHIFT=0 → 0 дБ (всегда открыт)
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  uint32_t floor;         /* текущая оценка шума                */
  uint8_t  rise_shift;    /* τ_rise = 2^rise_shift блоков       */
  uint8_t  snr_on_shift;  /* порог открытия  (рекоменд. 1)      */
  uint8_t  snr_off_shift; /* порог закрытия  (рекоменд. 0)      */
  bool     open;          /* состояние сквелча                  */
} NoiseFloor;

void nf_init(NoiseFloor *nf, uint8_t rise_shift,
             uint8_t snr_on, uint8_t snr_off);
/* power: суммарная мощность (OOK: power[0]; FSK: power[0]+power[1]) */
bool nf_process(NoiseFloor *nf, uint32_t power);

/* ═══════════════════════════════════════════════════════════════
 *  [4] Baud detector — GCD-гистограмма (как в ook.c, но на Fs/N)
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  uint32_t hist[GOERTZEL_HIST_N];
  uint32_t pulse_count;
  uint32_t run_cnt;
  bool     last_bit;
  uint32_t spb;    /* samples-per-bit на частоте Fs/block_n */
  uint32_t votes;
} BaudDet;

void     bd_init(BaudDet *b);
uint32_t bd_push(BaudDet *b, bool bit); /* → spb или 0 */

/* ═══════════════════════════════════════════════════════════════
 *  [5] Bit sampler
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  uint32_t cnt;
  uint32_t ones;
} BitSampler;

void bsamp_init(BitSampler *s);
/* true → бит готов, *out заполнен */
bool bsamp_push(BitSampler *s, bool bit, uint32_t spb, bool *out);

/* ═══════════════════════════════════════════════════════════════
 *  [6] Framer — SOF / EOF, сборка в байты
 * ═══════════════════════════════════════════════════════════════ */
typedef void (*PktStartFn)(void);
typedef void (*PktDataFn)(const uint8_t *data, uint16_t nbytes);

typedef enum { FRAME_IDLE = 0, FRAME_RX } FrameState;

typedef struct {
  FrameState state;
  uint8_t    buf[GOERTZEL_MAX_BITS / 8];
  uint32_t   bit_idx;
  uint32_t   idle_cnt;
  uint32_t   idle_max; /* idle_max bit-периодов → EOF */
} Framer;

void framer_init(Framer *f, uint32_t idle_bits);
void framer_push(Framer *f, bool bit_ready, bool bit_val, bool squelch_open,
                 PktStartFn on_start, PktDataFn on_pkt);

/* ═══════════════════════════════════════════════════════════════
 *  Полный конвейер
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  ToneMode mode;
  uint16_t block_n;   /* размер блока Goertzel (сэмплов АЦП)  */
  uint16_t block_cnt; /* счётчик сэмплов в текущем блоке      */
  int32_t  dc_est;    /* оценка DC-смещения АЦП (slow IIR)    */

  Goertzel   g[2];    /* g[0]=mark/OOK, g[1]=space (FSK)      */
  NoiseFloor nf;
  bool       bit_val; /* текущее бинарное решение              */

  BaudDet    baud;
  BitSampler sampler;
  Framer     framer;
} ToneDemod;

/* Глобальный экземпляр (поля публичны для отображения на экране) */
extern ToneDemod g_tone;

extern PktStartFn toneStartHandler;
extern PktDataFn  tonePacketHandler;

/* ── Публичный API ──────────────────────────────────────────── */

/*
 * Инициализация для OOK с субнесущей:
 *   tone_hz  — частота субнесущей
 *   block_n  — размер блока (≤ бит_период_в_сэмплах)
 *
 *   Пример (200 бод, субнесущая 1кГц):
 *     бит = 9600/200 = 48 сэмпла → block_n=24 (2 блока на бит)
 *     tone_demod_init_ook(1000.0f, 24);
 */
void tone_demod_init_ook(float tone_hz, uint16_t block_n);

/*
 * Инициализация для 2-FSK:
 *   mark_hz  — частота для бит=1
 *   space_hz — частота для бит=0
 *   block_n  — размер блока (≤ бит_период_в_сэмплах)
 *
 *   Примеры:
 *     Bell 202 / APRS (1200 бод):
 *       бит = 8 сэмплов → block_n=8
 *       tone_demod_init_fsk(1200.0f, 2200.0f, 8);
 *
 *     Bell 103 (300 бод):
 *       бит = 32 сэмпла → block_n=16
 *       tone_demod_init_fsk(1270.0f, 1070.0f, 16);
 */
void tone_demod_init_fsk(float mark_hz, float space_hz, uint16_t block_n);

/* Зарегистрировать как AudioSink */
void tone_demod_sink(const uint16_t *buf, uint32_t n);

/* Текущий битрейт в бодах (0 если не определён) */
uint32_t tone_demod_get_bitrate(void);
