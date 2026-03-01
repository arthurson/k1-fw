/*
 * ook.c — OOK DSP pipeline implementation
 *
 * Конвейер сигнальной обработки:
 *
 *   ADC (uint16, 9600 Hz)
 *    │
 *    ▼  ook_env_process()
 *   [1] Envelope  ─── пиковый детектор огибающей
 *    │  out: env (int32)
 *    │
 *    ▼  ook_squelch_process()
 *   [2] Squelch   ─── трекер шумового пола, SNR-гейт
 *    │  out: squelch.open (bool), squelch.floor (int32)
 *    │
 *    ▼  ook_carrier_process()
 *   [3] Carrier   ─── мгновенный компаратор несущей
 *    │  out: carrier (bool)
 *    │
 *    ▼  ook_baud_process()
 *   [4] Baud      ─── автодетект bitrate по GCD гистограммы
 *    │  out: spb (uint32, samples-per-bit; 0 = неизвестно)
 *    │
 *    ▼  ook_sampler_process()
 *   [5] Sampler   ─── накопитель + решение большинством
 *    │  out: bit_ready, bit_val
 *    │
 *    ▼  ook_framer_process()
 *   [6] Framer    ─── SOF/EOF, сборка байтов
 *    │
 *    ├─► ookStartHandler()              — начало пакета
 *    └─► ookHandler(data, nbytes)       — конец пакета
 */

#include "ook.h"
#include "../driver/uart.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 *  ПАРАМЕТРЫ КОНВЕЙЕРА ПО УМОЛЧАНИЮ
 *  (передаются в ook_init через ook_*_init)
 * ═══════════════════════════════════════════════════════════════
 *
 *  Envelope decay:
 *    DECAY_SHIFT=8 → τ≈27мс
 *    Должен быть > длительности самого длинного нулевого бита.
 *    При 300 бод нулевой бит = 3.3мс → 27мс >> 3.3мс ✓
 *    При 100 бод нулевой бит = 10мс  → 27мс > 10мс  ✓
 *    Увеличь если теряются нулевые биты.
 *
 *  Squelch noise-floor rise:
 *    RISE_SHIFT=11 → τ_rise≈213мс
 *    Пол медленно ползёт вверх при длинном сигнале — это нормально,
 *    т.к. мгновенно падает обратно после окончания пакета.
 *
 *  Squelch SNR:
 *    SNR_ON_SHIFT=0  → открыть при env > floor*2  (6 дБ)
 *    SNR_OFF_SHIFT=1 → закрыть при env < floor*1.5 (3.5 дБ)
 *    Уменьши если сигнал слабый (но будет больше ложных срабатываний).
 *
 *  Baud: HIST_MIN_PULSES, SPB_CONFIRM_VOTES — чем меньше, тем
 *  быстрее захват битрейта, но менее стабильно.
 *
 *  Framer IDLE_BITS=12 → конец пакета после 12 пустых битовых периодов.
 * ═══════════════════════════════════════════════════════════════ */

#define ENV_DECAY_SHIFT     8u
#define SQL_RISE_SHIFT      11u
#define SQL_SNR_ON_SHIFT    0u   /* env > floor << 1        */
#define SQL_SNR_OFF_SHIFT   1u   /* env < floor + floor>>1  */
#define BAUD_MIN_PULSES     4u
#define BAUD_SPB_MIN        4u
#define BAUD_SPB_MAX        192u
#define BAUD_GCD_TOL        2u
#define BAUD_CONFIRM_VOTES  1u
#define FRAMER_IDLE_BITS    12u

/* ═══════════════════════════════════════════════════════════════
 *  ГЛОБАЛЬНОЕ СОСТОЯНИЕ
 * ═══════════════════════════════════════════════════════════════ */

OOK_Pipeline g_ook;

OOK_StartFn  ookStartHandler = NULL;
OOK_PacketFn ookHandler      = NULL;

/* ═══════════════════════════════════════════════════════════════
 *  СТУПЕНЬ 1 — ENVELOPE
 * ═══════════════════════════════════════════════════════════════ */

void ook_env_init(OOK_Envelope *e, uint8_t decay_shift) {
  e->peak        = 0;
  e->decay_shift = decay_shift;
}

int32_t ook_env_process(OOK_Envelope *e, int32_t x) {
  if (x > e->peak) {
    /* Мгновенный подъём: первый же высокий сэмпл OOK=1 захватывается */
    e->peak = x;
  } else if (e->peak > 0) {
    /* Медленный спад: держит пик на время нулевых OOK-битов */
    e->peak -= (int32_t)((uint32_t)e->peak >> e->decay_shift);
  }
  return e->peak;
}

/* ═══════════════════════════════════════════════════════════════
 *  СТУПЕНЬ 2 — SQUELCH
 * ═══════════════════════════════════════════════════════════════ */

void ook_squelch_init(OOK_Squelch *sq, uint8_t rise_shift,
                      uint8_t snr_on_sh, uint8_t snr_off_sh) {
  sq->floor      = 0;
  sq->rise_shift = rise_shift;
  sq->snr_on_sh  = snr_on_sh;
  sq->snr_off_sh = snr_off_sh;
  sq->open       = false;
}

bool ook_squelch_process(OOK_Squelch *sq, int32_t env) {
  /* Шумовой пол: мгновенный спад, медленный подъём.
   *
   * Логика:
   *   Без сигнала: env ≈ noise → floor ≈ noise.
   *   При сигнале: env >> noise → floor медленно растёт.
   *   После сигнала: env падает к noise → floor мгновенно падает обратно.
   *
   * Это позволяет floor всегда оценивать именно уровень шума. */
  if (env < sq->floor)
    sq->floor = env;                                    /* мгновенный спад */
  else if (sq->floor < env)
    sq->floor += (env - sq->floor) >> sq->rise_shift;  /* медленный подъём */

  /* SNR-гейт с гистерезисом:
   *   Открыть:  env > floor << snr_on_sh   (= floor * 2^snr_on_sh)
   *   Закрыть:  env < floor + (floor >> snr_off_sh)
   *
   *   snr_on_sh=0:  открыть при env > floor * 2  (6 дБ над шумом)
   *   snr_off_sh=1: закрыть при env < floor * 1.5 (3.5 дБ над шумом)
   *
   *   Гистерезис 2.5 дБ исключает дребезг на границе сигнал/шум. */
  int32_t thr_on  = sq->floor  << sq->snr_on_sh;
  int32_t thr_off = sq->floor + (sq->floor >> sq->snr_off_sh);

  if (!sq->open && env > thr_on)
    sq->open = true;
  if ( sq->open && env < thr_off)
    sq->open = false;

  return sq->open;
}

/* ═══════════════════════════════════════════════════════════════
 *  СТУПЕНЬ 3 — CARRIER
 * ═══════════════════════════════════════════════════════════════ */

void ook_carrier_init(OOK_Carrier *c) { c->carrier = false; }

bool ook_carrier_process(OOK_Carrier *c, int32_t env, int32_t floor,
                         bool squelch_open) {
  if (!squelch_open) {
    /* Нет сигнала — несущая точно выключена */
    c->carrier = false;
    return false;
  }

  /* Пороги: floor*2 и floor*1.5 дают гистерезис внутри полосы сигнала.
   * Это не дублирует squelch: squelch работает на масштабе пакета,
   * carrier — на масштабе бита. */
  int32_t thr_on  = floor << 1;                /* floor * 2   */
  int32_t thr_off = floor + (floor >> 1);      /* floor * 1.5 */

  if (!c->carrier && env > thr_on)
    c->carrier = true;
  if ( c->carrier && env < thr_off)
    c->carrier = false;

  return c->carrier;
}

/* ═══════════════════════════════════════════════════════════════
 *  СТУПЕНЬ 4 — BAUD DETECT
 * ═══════════════════════════════════════════════════════════════ */

void ook_baud_init(OOK_Baud *b) { memset(b, 0, sizeof(*b)); }

static uint32_t gcd_approx(uint32_t a, uint32_t b) {
  while (b > 1u) {
    uint32_t r = a % b;
    if (r < BAUD_GCD_TOL || r > b - BAUD_GCD_TOL) {
      b = 0u;
      break;
    }
    a = b;
    b = r;
  }
  return a;
}

static uint32_t histogram_gcd(const uint32_t *hist) {
  uint32_t g = 0u;
  for (uint32_t i = BAUD_SPB_MIN; i < OOK_HIST_SIZE; i++) {
    if (!hist[i])
      continue;
    g = g ? gcd_approx(i, g) : i;
    if (g < BAUD_SPB_MIN)
      return 0u;
  }
  return g;
}

uint32_t ook_baud_process(OOK_Baud *b, bool carrier) {
  /* Пока carrier не изменился — просто считаем длину текущего импульса */
  if (carrier == b->last_carrier) {
    b->run_cnt++;
    return (b->spb_votes >= BAUD_CONFIRM_VOTES) ? b->spb : 0u;
  }

  /* Переход — записываем длину предыдущего импульса в гистограмму */
  uint32_t len    = b->run_cnt;
  b->run_cnt      = 1u;
  b->last_carrier = carrier;

  if (len >= BAUD_SPB_MIN && len < OOK_HIST_SIZE) {
    b->hist[len]++;
    b->pulse_count++;
  }

  if (b->pulse_count < BAUD_MIN_PULSES)
    return 0u;

  /* Пересчитываем GCD каждые 4 перехода — не каждый раз, для скорости */
  if (b->pulse_count % 4u != 0u)
    return (b->spb_votes >= BAUD_CONFIRM_VOTES) ? b->spb : 0u;

  uint32_t c = histogram_gcd(b->hist);
  if (c >= BAUD_SPB_MIN && c <= BAUD_SPB_MAX) {
    if (c == b->spb)
      b->spb_votes++;
    else {
      b->spb       = c;
      b->spb_votes = 1u;
    }
  }
  return (b->spb_votes >= BAUD_CONFIRM_VOTES) ? b->spb : 0u;
}

uint32_t ook_baud_get_rate(const OOK_Baud *b) {
  if (b->spb_votes < BAUD_CONFIRM_VOTES || !b->spb)
    return 0u;
  return OOK_SAMPLE_RATE / b->spb;
}

/* ═══════════════════════════════════════════════════════════════
 *  СТУПЕНЬ 5 — SAMPLER
 * ═══════════════════════════════════════════════════════════════ */

void ook_sampler_init(OOK_Sampler *s) {
  s->cnt  = 0u;
  s->ones = 0u;
}

bool ook_sampler_process(OOK_Sampler *s, bool carrier, uint32_t spb,
                         bool *bit_out) {
  s->cnt++;
  if (carrier)
    s->ones++;

  if (s->cnt < spb)
    return false;

  /* Большинством голосов: если ones > half → бит=1 */
  *bit_out = (s->ones > spb / 2u);
  s->cnt   = 0u;
  s->ones  = 0u;
  return true;
}

/* ═══════════════════════════════════════════════════════════════
 *  СТУПЕНЬ 6 — FRAMER
 * ═══════════════════════════════════════════════════════════════ */

void ook_framer_init(OOK_Framer *f, uint32_t idle_bits) {
  memset(f, 0, sizeof(*f));
  f->idle_max = idle_bits;
}

static void framer_push_bit(OOK_Framer *f, bool one) {
  if (f->bit_idx >= OOK_MAX_BITS)
    return;
  uint32_t byte_i = f->bit_idx >> 3u;
  uint32_t bit_i  = 7u - (f->bit_idx & 7u); /* MSB первым */
  if (one) f->buf[byte_i] |=  (uint8_t)(1u << bit_i);
  else     f->buf[byte_i] &= ~(uint8_t)(1u << bit_i);
  f->bit_idx++;
}

static void framer_flush(OOK_Framer *f, OOK_PacketFn on_packet) {
  if (f->bit_idx >= 8u && on_packet)
    on_packet(f->buf, (uint16_t)((f->bit_idx + 7u) >> 3u));

  memset(f->buf, 0, sizeof(f->buf));
  f->bit_idx = 0u;
  f->idle_cnt = 0u;
  f->state   = OOK_FRAME_IDLE;
}

void ook_framer_process(OOK_Framer *f, bool bit_ready, bool bit_val,
                        bool carrier, OOK_StartFn on_start,
                        OOK_PacketFn on_packet) {
  /* Если сквелч закрылся (нет сигнала вообще) — сбрасываем пакет */
  if (!carrier && f->state == OOK_FRAME_RX) {
    if (++f->idle_cnt >= f->idle_max) {
      /* EOF: сигнал пропал — отдаём накопленный пакет */
      framer_flush(f, on_packet);
      return;
    }
  }

  if (!bit_ready)
    return;

  switch (f->state) {

  case OOK_FRAME_IDLE:
    if (bit_val) {
      /* SOF: первый единичный бит после тишины */
      f->state    = OOK_FRAME_RX;
      f->idle_cnt = 0u;
      if (on_start)
        on_start();
      framer_push_bit(f, 1u);
    }
    break;

  case OOK_FRAME_RX:
    if (bit_val) {
      f->idle_cnt = 0u;   /* несущая живёт — сбрасываем счётчик тишины */
    } else {
      f->idle_cnt++;
      if (f->idle_cnt >= f->idle_max) {
        /* EOF: достаточно пустых битов — пакет завершён */
        framer_flush(f, on_packet);
        return;
      }
    }

    if (f->bit_idx >= OOK_MAX_BITS) {
      /* Переполнение буфера — сбрасываем без коллбека */
      framer_flush(f, NULL);
      return;
    }

    framer_push_bit(f, bit_val);
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════
 *  ГЛАВНЫЙ КОЛБЕК: ook_sink
 *  Принимает блок ADC-сэмплов и прогоняет через все ступени.
 * ═══════════════════════════════════════════════════════════════ */

void ook_sink(const uint16_t *buf, uint32_t n) {
  OOK_Pipeline *p = &g_ook;

  /* Отладочные аккумуляторы */
  static uint32_t dbg_n         = 0u;
  static int32_t  dbg_env_max   = 0;
  static int32_t  dbg_floor_max = 0;

  for (uint32_t i = 0u; i < n; i++) {
    int32_t x = (int32_t)(uint32_t)buf[i];

    /* ── 1. Огибающая ───────────────────────────────────────── */
    int32_t env = ook_env_process(&p->env, x);

    /* ── 2. Сквелч (сигнал/шум) ─────────────────────────────── */
    bool sq_open = ook_squelch_process(&p->squelch, env);

    /* ── 3. Детектор несущей ────────────────────────────────── */
    bool carrier = ook_carrier_process(&p->carrier, env, p->squelch.floor,
                                       sq_open);

    /* ── 4. Автодетект битрейта ─────────────────────────────── */
    uint32_t spb = ook_baud_process(&p->baud, carrier);

    /* Без spb — нельзя семплировать и фреймировать */
    if (!spb)
      goto dbg;

    /* ── 5. Битовый семплер ─────────────────────────────────── */
    bool bit_val   = false;
    bool bit_ready = ook_sampler_process(&p->sampler, carrier, spb, &bit_val);

    /* ── 6. Фреймер (SOF/EOF) ───────────────────────────────── */
    ook_framer_process(&p->framer, bit_ready, bit_val, carrier,
                       ookStartHandler, ookHandler);

  dbg:
    if (env         > dbg_env_max)   dbg_env_max   = env;
    if (p->squelch.floor > dbg_floor_max) dbg_floor_max = p->squelch.floor;
  }

  /* Лог раз в секунду
   *
   * Интерпретация:
   *   env_max  — пиковая огибающая за секунду:
   *              в тишине ≈ 50–200, при сигнале ≈ 500–4095
   *   floor    — оценка шума (всегда ≤ env_max)
   *   snr      — env_max / floor (условный, для ориентира)
   *   squelch  — 1 если хоть раз открылся за секунду
   *   baud     — определённый битрейт, 0 если неизвестен
   *
   * Если squelch=0 при нажатой кнопке:
   *   env_max слишком низкий → уменьши SQL_SNR_ON_SHIFT (например до -1)
   *   или проверь усиление BK4819.
   */
  dbg_n += n;
  if (dbg_n >= OOK_SAMPLE_RATE) {
    dbg_n = 0u;
    int32_t snr_x10 = dbg_floor_max
                        ? (dbg_env_max * 10) / dbg_floor_max
                        : 0;
    Log("env_max=%ld floor=%ld snr=%ld.%ldx | sq=%d carrier=%d baud=%lu",
        (long)dbg_env_max,
        (long)dbg_floor_max,
        (long)(snr_x10 / 10),
        (long)(snr_x10 % 10),
        (int)p->squelch.open,
        (int)p->carrier.carrier,
        (unsigned long)ook_baud_get_rate(&p->baud));
    Log("  spb=%lu votes=%lu pulses=%lu | frame=%s bits=%lu",
        (unsigned long)p->baud.spb,
        (unsigned long)p->baud.spb_votes,
        (unsigned long)p->baud.pulse_count,
        p->framer.state == OOK_FRAME_RX ? "RX" : "IDLE",
        (unsigned long)p->framer.bit_idx);
    dbg_env_max   = 0;
    dbg_floor_max = 0;
  }
}

/* ═══════════════════════════════════════════════════════════════
 *  ПУБЛИЧНЫЙ API
 * ═══════════════════════════════════════════════════════════════ */

void ook_init(void) {
  ook_env_init    (&g_ook.env,     ENV_DECAY_SHIFT);
  ook_squelch_init(&g_ook.squelch, SQL_RISE_SHIFT,
                                   SQL_SNR_ON_SHIFT, SQL_SNR_OFF_SHIFT);
  ook_carrier_init(&g_ook.carrier);
  ook_baud_init   (&g_ook.baud);
  ook_sampler_init(&g_ook.sampler);
  ook_framer_init (&g_ook.framer, FRAMER_IDLE_BITS);
}

void ook_reset(void) {
  /* Сохраняем накопленные состояния огибающей и пола —
   * они уже сошлись и не должны прыгать после сброса. */
  int32_t  saved_peak  = g_ook.env.peak;
  int32_t  saved_floor = g_ook.squelch.floor;

  ook_init();   /* сброс всех ступеней */

  g_ook.env.peak      = saved_peak;
  g_ook.squelch.floor = saved_floor;
}

uint32_t ook_get_bitrate(void) {
  return ook_baud_get_rate(&g_ook.baud);
}

