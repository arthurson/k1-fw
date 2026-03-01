/*
 * goertzel.c — Goertzel-based tone demodulator
 *
 * Алгоритм Герцеля — DFT на одну частоту за O(N).
 *
 * Для каждого блока из N сэмплов:
 *   s[n] = x[n] + coef·s[n-1] − s[n-2]     (рекуррентность)
 *   power = s[N]² + s[N-1]² − coef·s[N]·s[N-1]
 *
 * Нет необходимости в FFT: только 2 умножения + 2 сложения на сэмпл.
 * coef = 2·cos(2π·f/Fs) вычисляется один раз при инициализации.
 * В hot-path — только int32 арифметика.
 */

#include "goertzel.h"
#include "../driver/uart.h"
#include <math.h>   /* cos(), M_PI — только для init, не в hot-path */
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 *  ДЕФОЛТНЫЕ ПАРАМЕТРЫ
 * ═══════════════════════════════════════════════════════════════ */

/* Шумовой пол: τ_rise = 2^NF_RISE_SHIFT блоков.
 * При block_n=8 и 9600 Гц: 1 блок = 0.83мс.
 * NF_RISE_SHIFT=10 → τ_rise ≈ 853 блока ≈ 710мс — медленный подъём. */
#define NF_RISE_SHIFT     10u

/* SNR-гейт:
 *   Открыть при  power > floor << SNR_ON_SHIFT   (floor * 2 = 6 дБ)
 *   Закрыть при  power < floor << SNR_OFF_SHIFT  (floor * 1 = 0 дБ)
 * Гистерезис 6 дБ — устойчиво к флуктуациям. */
#define SNR_ON_SHIFT      1u
#define SNR_OFF_SHIFT     0u

/* GCD детектор битрейта */
#define BD_MIN_PULSES     4u
#define BD_SPB_MIN        1u
#define BD_SPB_MAX        96u
#define BD_GCD_TOL        1u
#define BD_CONFIRM_VOTES  2u

/* Конец пакета после N пустых bit-периодов */
#define FRAMER_IDLE_BITS  12u

/* DC-трекер: IIR τ ≈ 2^DC_SHIFT / Fs */
#define DC_SHIFT          10u

/* ═══════════════════════════════════════════════════════════════
 *  ГЛОБАЛЬНЫЕ ОБЪЕКТЫ
 * ═══════════════════════════════════════════════════════════════ */

ToneDemod  g_tone;
PktStartFn toneStartHandler  = NULL;
PktDataFn  tonePacketHandler = NULL;

/* ═══════════════════════════════════════════════════════════════
 *  [1] GOERTZEL
 * ═══════════════════════════════════════════════════════════════ */

void goertzel_init(Goertzel *g, float freq_hz) {
  double angle  = 2.0 * M_PI * (double)freq_hz / (double)GOERTZEL_FS;
  g->coef_q12   = (int32_t)(2.0 * cos(angle) * 4096.0 + 0.5);
  g->s1 = g->s2 = 0;
  g->power      = 0u;
}

void goertzel_reset(Goertzel *g) {
  g->s1 = g->s2 = 0;
}

uint32_t goertzel_finish(Goertzel *g) {
  /* power = s1² + s2² − coef·s1·s2
   *
   * Это вещественная часть |X(f)|², масштабированная на N²/4.
   * Численно: при N=8, |x|≤128 → s1,s2 ≤ 1024.
   *   s1² ≤ 1M, coef ≤ 8192, cross = (1024*8192>>12)*1024 = 2048*1024 = 2M
   *   Всё умещается в int32 с запасом.                            */
  int32_t cross = ((int32_t)((g->s1 * g->coef_q12) >> 12)) * g->s2;
  int32_t p     = g->s1 * g->s1 + g->s2 * g->s2 - cross;
  g->power      = (p > 0) ? (uint32_t)p : 0u;
  g->s1 = g->s2 = 0; /* сброс для следующего блока */
  return g->power;
}

/* ═══════════════════════════════════════════════════════════════
 *  [2] NOISE FLOOR
 * ═══════════════════════════════════════════════════════════════ */

void nf_init(NoiseFloor *nf, uint8_t rise_shift,
             uint8_t snr_on, uint8_t snr_off) {
  nf->floor        = 0u;
  nf->rise_shift   = rise_shift;
  nf->snr_on_shift = snr_on;
  nf->snr_off_shift= snr_off;
  nf->open         = false;
}

bool nf_process(NoiseFloor *nf, uint32_t power) {
  /* Шумовой пол: мгновенный спад, медленный подъём.
   *
   * Без сигнала:  power ≈ шум → floor ≈ шум.
   * При сигнале:  power >> шум → floor медленно растёт,
   *               но сквелч уже открыт — это нормально.
   * После пакета: power падает → floor мгновенно спускается к шуму. */
  if (power < nf->floor)
    nf->floor = power;
  else
    nf->floor += (power - nf->floor) >> nf->rise_shift;

  /* SNR-гейт с гистерезисом:
   *   Открыть:  power > floor << snr_on_shift
   *   Закрыть:  power < floor << snr_off_shift              */
  uint32_t thr_on  = nf->floor << nf->snr_on_shift;
  uint32_t thr_off = nf->floor << nf->snr_off_shift;

  if (!nf->open && power > thr_on)
    nf->open = true;
  if ( nf->open && power < thr_off)
    nf->open = false;

  return nf->open;
}

/* ═══════════════════════════════════════════════════════════════
 *  [4] BAUD DETECTOR
 * ═══════════════════════════════════════════════════════════════ */

void bd_init(BaudDet *b) { memset(b, 0, sizeof(*b)); }

static uint32_t gcd2(uint32_t a, uint32_t b) {
  while (b > 1u) {
    uint32_t r = a % b;
    if (r < BD_GCD_TOL || r > b - BD_GCD_TOL) { b = 0u; break; }
    a = b; b = r;
  }
  return a;
}

static uint32_t hist_gcd(const uint32_t *hist) {
  uint32_t g = 0u;
  for (uint32_t i = BD_SPB_MIN; i < GOERTZEL_HIST_N; i++) {
    if (!hist[i]) continue;
    g = g ? gcd2(i, g) : i;
    if (g < BD_SPB_MIN) return 0u;
  }
  return g;
}

uint32_t bd_push(BaudDet *b, bool bit) {
  if (bit == b->last_bit) {
    b->run_cnt++;
    return (b->votes >= BD_CONFIRM_VOTES) ? b->spb : 0u;
  }

  uint32_t len = b->run_cnt;
  b->run_cnt   = 1u;
  b->last_bit  = bit;

  if (len >= BD_SPB_MIN && len < GOERTZEL_HIST_N) {
    b->hist[len]++;
    b->pulse_count++;
  }

  if (b->pulse_count < BD_MIN_PULSES) return 0u;
  if (b->pulse_count % 4u)
    return (b->votes >= BD_CONFIRM_VOTES) ? b->spb : 0u;

  uint32_t c = hist_gcd(b->hist);
  if (c >= BD_SPB_MIN && c <= BD_SPB_MAX) {
    if (c == b->spb) b->votes++;
    else { b->spb = c; b->votes = 1u; }
  }
  return (b->votes >= BD_CONFIRM_VOTES) ? b->spb : 0u;
}

/* ═══════════════════════════════════════════════════════════════
 *  [5] BIT SAMPLER
 * ═══════════════════════════════════════════════════════════════ */

void bsamp_init(BitSampler *s) { s->cnt = s->ones = 0u; }

bool bsamp_push(BitSampler *s, bool bit, uint32_t spb, bool *out) {
  s->cnt++;
  if (bit) s->ones++;
  if (s->cnt < spb) return false;
  *out = (s->ones > spb / 2u);
  s->cnt = s->ones = 0u;
  return true;
}

/* ═══════════════════════════════════════════════════════════════
 *  [6] FRAMER
 * ═══════════════════════════════════════════════════════════════ */

void framer_init(Framer *f, uint32_t idle_bits) {
  memset(f, 0, sizeof(*f));
  f->idle_max = idle_bits;
}

static void frame_push_bit(Framer *f, bool one) {
  if (f->bit_idx >= GOERTZEL_MAX_BITS) return;
  uint32_t bi = f->bit_idx >> 3u;
  uint32_t bp = 7u - (f->bit_idx & 7u);
  if (one) f->buf[bi] |=  (uint8_t)(1u << bp);
  else     f->buf[bi] &= ~(uint8_t)(1u << bp);
  f->bit_idx++;
}

static void frame_flush(Framer *f, PktDataFn on_pkt) {
  if (f->bit_idx >= 8u && on_pkt)
    on_pkt(f->buf, (uint16_t)((f->bit_idx + 7u) >> 3u));
  memset(f->buf, 0, sizeof(f->buf));
  f->bit_idx = f->idle_cnt = 0u;
  f->state   = FRAME_IDLE;
}

void framer_push(Framer *f, bool bit_ready, bool bit_val, bool squelch_open,
                 PktStartFn on_start, PktDataFn on_pkt) {
  /* Принудительный EOF если сквелч закрылся */
  if (!squelch_open && f->state == FRAME_RX) {
    frame_flush(f, on_pkt);
    return;
  }
  if (!bit_ready) return;

  switch (f->state) {
  case FRAME_IDLE:
    if (bit_val) {
      /* SOF: первая единица после тишины */
      f->state = FRAME_RX;
      f->idle_cnt = 0u;
      if (on_start) on_start();
      frame_push_bit(f, 1u);
    }
    break;

  case FRAME_RX:
    if (!bit_val) {
      if (++f->idle_cnt >= f->idle_max) {
        /* EOF: достаточно нулевых bit-периодов */
        frame_flush(f, on_pkt);
        return;
      }
    } else {
      f->idle_cnt = 0u;
    }
    if (f->bit_idx >= GOERTZEL_MAX_BITS) {
      frame_flush(f, NULL); /* переполнение — сброс без коллбека */
      return;
    }
    frame_push_bit(f, bit_val);
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════
 *  ГЛАВНЫЙ КОЛБЕК
 * ═══════════════════════════════════════════════════════════════ */

void tone_demod_sink(const uint16_t *buf, uint32_t n) {
  ToneDemod *t = &g_tone;

  /* Счётчики для лога (раз в секунду) */
  static uint32_t dbg_n       = 0u;
  static uint32_t dbg_pw0_max = 0u;
  static uint32_t dbg_pw1_max = 0u;

  for (uint32_t i = 0u; i < n; i++) {
    /* ── 0. DC-удаление ─────────────────────────────────────────
     *
     * Медленный IIR следит за постоянной составляющей АЦП.
     * dc_est хранит DC * 2^DC_SHIFT.
     * Вычитаем DC → центрируем сигнал вокруг нуля.
     * Масштабируем >> GOERTZEL_SCALE → ±128.
     *
     * Без DC-удаления мощность на нулевой частоте "протечёт"
     * в целевые тоны через конечный размер окна.                */
    t->dc_est += ((int32_t)(uint32_t)buf[i] << DC_SHIFT) - t->dc_est;
    int32_t x = ((int32_t)(uint32_t)buf[i] - (t->dc_est >> DC_SHIFT))
                >> GOERTZEL_SCALE;

    /* ── 1. Накапливаем в Goertzel-фильтр(ы) ────────────────── */
    goertzel_push(&t->g[0], x);
    if (t->mode == TONE_FSK)
      goertzel_push(&t->g[1], x);

    /* ── 2. Каждые block_n сэмплов — принимаем решение ─────── */
    if (++t->block_cnt < t->block_n)
      continue;
    t->block_cnt = 0u;

    uint32_t pw0 = goertzel_finish(&t->g[0]);
    uint32_t pw1 = (t->mode == TONE_FSK) ? goertzel_finish(&t->g[1]) : 0u;

    if (pw0 > dbg_pw0_max) dbg_pw0_max = pw0;
    if (pw1 > dbg_pw1_max) dbg_pw1_max = pw1;

    /* ── 3. Noise floor + squelch ───────────────────────────────
     *
     * OOK: суммарная мощность = pw0.
     * FSK: суммарная = pw0 + pw1 (хотя бы один тон всегда есть). */
    uint32_t total_pw = (t->mode == TONE_FSK) ? pw0 + pw1 : pw0;
    bool sq = nf_process(&t->nf, total_pw);

    /* ── 4. Бинарное решение (carrier / bit) ───────────────────
     *
     * OOK: несущая есть → pw0 выше шумового пола (уже проверено sq).
     *      sq=1 → carrier=1; sq=0 → carrier=0.
     *
     * FSK: сквелч только включает/выключает декодер целиком.
     *      Бит = (pw0 > pw1): более мощный тон выигрывает.
     *      Простое сравнение работает даже в шуме — относительная
     *      мощность устойчива к изменению уровня сигнала.         */
    bool bit_val;
    if (t->mode == TONE_OOK) {
      bit_val = sq; /* OOK: сигнал есть = 1 */
    } else {
      bit_val = sq && (pw0 > pw1); /* FSK: mark(pw0) > space(pw1) */
    }
    t->bit_val = bit_val;

    /* ── 5. Автодетект битрейта ─────────────────────────────── */
    uint32_t spb = bd_push(&t->baud, bit_val);
    if (!spb) continue;

    /* ── 6. Битовый семплер ─────────────────────────────────── */
    bool bit_out   = false;
    bool bit_ready = bsamp_push(&t->sampler, bit_val, spb, &bit_out);

    /* ── 7. Фреймер (SOF / EOF) ─────────────────────────────── */
    framer_push(&t->framer, bit_ready, bit_out, sq,
                toneStartHandler, tonePacketHandler);
  }

  /* ── Лог раз в секунду ──────────────────────────────────────
   *
   * OOK интерпретация:
   *   pw0_max в тишине: baseline шума. floor должен быть ≈ baseline.
   *   pw0_max при сигнале: рабочий уровень. SNR = pw/floor.
   *
   * FSK интерпретация:
   *   pw0_max ≈ pw1_max при отсутствии сигнала — это нормально.
   *   При сигнале один из них будет значительно больше другого.
   *   Если pw0_max ≈ pw1_max всегда → нет сигнала или неверный block_n.
   *
   * Если sq=0 при наличии сигнала:
   *   → floor слишком высокий (шум большой)
   *   → попробуй SNR_ON_SHIFT=0 (тогда любой сигнал > floor открывает)
   *
   * Если baud=0 долго:
   *   → bit_val не переключается → проверь pw0/pw1 и floor
   *   → возможно block_n слишком большой (больше длительности бита)  */
  dbg_n += n;
  if (dbg_n >= GOERTZEL_FS) {
    dbg_n = 0u;
    const char *mode_str = (t->mode == TONE_OOK) ? "OOK" : "FSK";
    Log("[%s N=%u] pw0=%lu pw1=%lu floor=%lu | sq=%d bit=%d baud=%lu",
        mode_str, (unsigned)t->block_n,
        (unsigned long)dbg_pw0_max,
        (unsigned long)dbg_pw1_max,
        (unsigned long)t->nf.floor,
        (int)t->nf.open,
        (int)t->bit_val,
        (unsigned long)tone_demod_get_bitrate());
    Log("  spb=%lu votes=%lu pulses=%lu | frame=%s bits=%lu",
        (unsigned long)t->baud.spb,
        (unsigned long)t->baud.votes,
        (unsigned long)t->baud.pulse_count,
        t->framer.state == FRAME_RX ? "RX" : "IDLE",
        (unsigned long)t->framer.bit_idx);
    dbg_pw0_max = dbg_pw1_max = 0u;
  }
}

/* ═══════════════════════════════════════════════════════════════
 *  ПУБЛИЧНЫЙ API
 * ═══════════════════════════════════════════════════════════════ */

void tone_demod_init_ook(float tone_hz, uint16_t block_n) {
  memset(&g_tone, 0, sizeof(g_tone));
  g_tone.mode    = TONE_OOK;
  g_tone.block_n = block_n ? block_n : 48u;
  goertzel_init(&g_tone.g[0], tone_hz);
  nf_init(&g_tone.nf, NF_RISE_SHIFT, SNR_ON_SHIFT, SNR_OFF_SHIFT);
  bd_init(&g_tone.baud);
  bsamp_init(&g_tone.sampler);
  framer_init(&g_tone.framer, FRAMER_IDLE_BITS);

  Log("ToneDemod OOK: tone=%.0f Hz, N=%u, Fs_dec=%u Hz",
      (double)tone_hz, (unsigned)g_tone.block_n,
      (unsigned)(GOERTZEL_FS / g_tone.block_n));
}

void tone_demod_init_fsk(float mark_hz, float space_hz, uint16_t block_n) {
  memset(&g_tone, 0, sizeof(g_tone));
  g_tone.mode    = TONE_FSK;
  g_tone.block_n = block_n ? block_n : 8u;
  goertzel_init(&g_tone.g[0], mark_hz);
  goertzel_init(&g_tone.g[1], space_hz);
  nf_init(&g_tone.nf, NF_RISE_SHIFT, SNR_ON_SHIFT, SNR_OFF_SHIFT);
  bd_init(&g_tone.baud);
  bsamp_init(&g_tone.sampler);
  framer_init(&g_tone.framer, FRAMER_IDLE_BITS);

  Log("ToneDemod FSK: mark=%.0f Hz, space=%.0f Hz, N=%u, Fs_dec=%u Hz",
      (double)mark_hz, (double)space_hz, (unsigned)g_tone.block_n,
      (unsigned)(GOERTZEL_FS / g_tone.block_n));
}

uint32_t tone_demod_get_bitrate(void) {
  if (g_tone.baud.votes < BD_CONFIRM_VOTES || !g_tone.baud.spb)
    return 0u;
  /* Битрейт = (Fs / block_n) / spb */
  return (GOERTZEL_FS / g_tone.block_n) / g_tone.baud.spb;
}
