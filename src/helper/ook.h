/*
 * ook.h — OOK DSP pipeline
 *
 * Ступени (каждая — независимая struct + process-функция):
 *
 *   [1] OOK_Envelope  — пиковый детектор огибающей
 *   [2] OOK_Squelch   — трекер шумового пола + SNR-гейт
 *   [3] OOK_Carrier   — компаратор несущей с гистерезисом
 *   [4] OOK_Baud      — автодетект битрейта (GCD-гистограмма)
 *   [5] OOK_Sampler   — битовый семплер
 *   [6] OOK_Framer    — сборщик пакетов (SOF/EOF)
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#define OOK_SAMPLE_RATE 9600u
#define OOK_MAX_BITS    256u
#define OOK_HIST_SIZE   256u

/* ═══════════════════════════════════════════════════════════════
 *  Ступень 1 — Envelope: пиковый детектор огибающей
 *
 *  Мгновенный подъём к x, медленный экспоненциальный спад.
 *  Выход: текущая амплитуда огибающей.
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  int32_t  peak;         /* текущее значение огибающей          */
  uint8_t  decay_shift;  /* τ_decay = 2^decay_shift / Fs        */
} OOK_Envelope;

/* decay_shift=8 → τ≈27мс (держит пик через нулевые биты OOK)  */
void    ook_env_init(OOK_Envelope *e, uint8_t decay_shift);
int32_t ook_env_process(OOK_Envelope *e, int32_t x);

/* ═══════════════════════════════════════════════════════════════
 *  Ступень 2 — Squelch: шумовой пол + SNR-порог
 *
 *  floor: мгновенный спад к минимуму, медленный подъём.
 *  Squelch открывается когда envelope > floor * (1 + 2^-snr_shift).
 *    snr_shift=0 → порог = 2 × floor (6 дБ)
 *    snr_shift=1 → порог = 1.5 × floor (3.5 дБ)
 *
 *  Гистерезис: открывается при SNR_ON, закрывается при SNR_OFF.
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  int32_t  floor;        /* оценка шумового пола                */
  uint8_t  rise_shift;   /* τ_rise = 2^rise_shift / Fs          */
  uint8_t  snr_on_sh;    /* порог открытия:    env > floor<<snr_on_sh  */
  uint8_t  snr_off_sh;   /* порог закрытия:    env < floor + (floor>>snr_off_sh) */
  bool     open;         /* текущее состояние сквелча           */
} OOK_Squelch;

/*
 * rise_shift=11  → τ_rise≈213мс — пол медленно ползёт вверх за сигналом
 * snr_on_sh=0    → открыть при env > floor*2  (6 дБ)
 * snr_off_sh=1   → закрыть при env < floor + floor/2 = floor*1.5 (3.5 дБ)
 */
void ook_squelch_init(OOK_Squelch *sq,
                      uint8_t rise_shift,
                      uint8_t snr_on_sh,
                      uint8_t snr_off_sh);
/* Возвращает true если сквелч открыт (сигнал есть) */
bool ook_squelch_process(OOK_Squelch *sq, int32_t env);

/* ═══════════════════════════════════════════════════════════════
 *  Ступень 3 — Carrier: мгновенный детектор несущей
 *
 *  Работает только пока squelch открыт.
 *  Пороги относительно шумового пола:
 *    ON  = floor * 2   (envelope пересекает вверх)
 *    OFF = floor * 1.5 (envelope падает ниже)
 *
 *  Когда squelch закрыт → carrier принудительно = false.
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  bool carrier;
} OOK_Carrier;

void ook_carrier_init(OOK_Carrier *c);
/* floor — из OOK_Squelch.floor; squelch_open — из OOK_Squelch.open */
bool ook_carrier_process(OOK_Carrier *c, int32_t env, int32_t floor,
                         bool squelch_open);

/* ═══════════════════════════════════════════════════════════════
 *  Ступень 4 — Baud: автодетект битрейта (GCD-гистограмма)
 *
 *  Накапливает длины импульсов и пауз несущей.
 *  НОД длин = период одного бита (spb, samples per bit).
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  uint32_t hist[OOK_HIST_SIZE];
  uint32_t pulse_count;
  uint32_t run_cnt;
  bool     last_carrier;
  uint32_t spb;        /* подтверждённый samples-per-bit         */
  uint32_t spb_votes;
} OOK_Baud;

void     ook_baud_init(OOK_Baud *b);
/* Возвращает spb если уже определён, иначе 0 */
uint32_t ook_baud_process(OOK_Baud *b, bool carrier);
/* Возвращает частоту в Гц, 0 если неизвестно */
uint32_t ook_baud_get_rate(const OOK_Baud *b);

/* ═══════════════════════════════════════════════════════════════
 *  Ступень 5 — Sampler: битовый семплер
 *
 *  Считает сэмплы внутри бита, решает большинством голосов.
 *  Возвращает true когда бит готов.
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  uint32_t cnt;   /* сэмплов в текущем периоде бита */
  uint32_t ones;  /* из них — единицы               */
} OOK_Sampler;

void ook_sampler_init(OOK_Sampler *s);
/* Возвращает true + заполняет *bit_out когда период бита закончился */
bool ook_sampler_process(OOK_Sampler *s, bool carrier, uint32_t spb,
                         bool *bit_out);

/* ═══════════════════════════════════════════════════════════════
 *  Ступень 6 — Framer: сборщик пакетов
 *
 *  SOF: первый бит=1 после IDLE
 *  EOF: IDLE_BITS подряд нулевых bit-периодов несущей
 *
 *  Коллбеки:
 *    on_start() — начало пакета (SOF)
 *    on_packet(data, nbytes) — конец пакета (EOF), передаёт данные
 * ═══════════════════════════════════════════════════════════════ */
typedef enum {
  OOK_FRAME_IDLE = 0,
  OOK_FRAME_RX,
} OOK_FrameState;

typedef void (*OOK_StartFn)(void);
typedef void (*OOK_PacketFn)(const uint8_t *data, uint16_t nbytes);

typedef struct {
  OOK_FrameState state;
  uint8_t        buf[OOK_MAX_BITS / 8];
  uint32_t       bit_idx;
  uint32_t       idle_cnt;   /* счётчик idle bit-периодов       */
  uint32_t       idle_max;   /* порог EOF (в bit-периодах)      */
} OOK_Framer;

/* idle_bits=12 → EOF после 12 пустых битовых периодов          */
void ook_framer_init(OOK_Framer *f, uint32_t idle_bits);
void ook_framer_process(OOK_Framer *f, bool bit_ready, bool bit_val,
                        bool carrier, OOK_StartFn on_start,
                        OOK_PacketFn on_packet);

/* ═══════════════════════════════════════════════════════════════
 *  Полный конвейер — агрегат всех ступеней
 *  (все поля публичны — можно читать для отображения на экране)
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
  OOK_Envelope env;
  OOK_Squelch  squelch;
  OOK_Carrier  carrier;
  OOK_Baud     baud;
  OOK_Sampler  sampler;
  OOK_Framer   framer;
} OOK_Pipeline;

/* Глобальный экземпляр конвейера — доступен для чтения извне */
extern OOK_Pipeline g_ook;

/* Пользовательские коллбеки */
extern OOK_StartFn  ookStartHandler;   /* вызывается на SOF (начало пакета) */
extern OOK_PacketFn ookHandler;        /* вызывается на EOF (конец пакета)  */

/* ── Публичный API ──────────────────────────────────────────── */
void     ook_init(void);
void     ook_reset(void);
void     ook_sink(const uint16_t *buf, uint32_t n);
uint32_t ook_get_bitrate(void);
