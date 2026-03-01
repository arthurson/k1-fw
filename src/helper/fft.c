#include "fft.h"
#include <string.h>

// ============================================================================
// Таблицы twiddle (Q15)
// cos(2*pi*k/128) * 32767
// sin(2*pi*k/128) * 32767
// ============================================================================

static const int16_t cos128[64] = {
    32767,  32728,  32609,  32412,  32137,  31785,  31356,  30852,
    30273,  29621,  28898,  28105,  27245,  26319,  25329,  24279,
    23170,  22005,  20787,  19519,  18204,  16846,  15446,  14010,
    12539,  11039,  9512,   7962,   6393,   4808,   3212,   1608,
    0,      -1608,  -3212,  -4808,  -6393,  -7962,  -9512,  -11039,
    -12539, -14010, -15446, -16846, -18204, -19519, -20787, -22005,
    -23170, -24279, -25329, -26319, -27245, -28105, -28898, -29621,
    -30273, -30852, -31356, -31785, -32137, -32412, -32609, -32728};

static const int16_t sin128[64] = {
    0,     1608,  3212,  4808,  6393,  7962,  9512,  11039, 12539, 14010, 15446,
    16846, 18204, 19519, 20787, 22005, 23170, 24279, 25329, 26319, 27245, 28105,
    28898, 29621, 30273, 30852, 31356, 31785, 32137, 32412, 32609, 32728, 32767,
    32728, 32609, 32412, 32137, 31785, 31356, 30852, 30273, 29621, 28898, 28105,
    27245, 26319, 25329, 24279, 23170, 22005, 20787, 19519, 18204, 16846, 15446,
    14010, 12539, 11039, 9512,  7962,  6393,  4808,  3212,  1608};

// ============================================================================
// Hann window (Q15)
// w(n) = 0.5*(1-cos(2πn/(N-1)))
// ============================================================================

static const int16_t hann128[128] = {
    0,     20,    80,    180,   320,   499,   717,   973,   1267,  1597,  1965,
    2367,  2803,  3273,  3775,  4308,  4870,  5461,  6078,  6721,  7387,  8075,
    8784,  9511,  10254, 11013, 11785, 12569, 13361, 14161, 14967, 15776, 16586,
    17396, 18203, 19006, 19803, 20591, 21369, 22135, 22886, 23622, 24340, 25039,
    25716, 26371, 27001, 27605, 28181, 28729, 29247, 29733, 30186, 30606, 30990,
    31340, 31652, 31927, 32164, 32363, 32522, 32642, 32722, 32762, 32762, 32722,
    32642, 32522, 32363, 32164, 31927, 31652, 31340, 30990, 30606, 30186, 29733,
    29247, 28729, 28181, 27605, 27001, 26371, 25716, 25039, 24340, 23622, 22886,
    22135, 21369, 20591, 19803, 19006, 18203, 17396, 16586, 15776, 14967, 14161,
    13361, 12569, 11785, 11013, 10254, 9511,  8784,  8075,  7387,  6721,  6078,
    5461,  4870,  4308,  3775,  3273,  2803,  2367,  1965,  1597,  1267,  973,
    717,   499,   320,   180,   80,    20,    0};

// ============================================================================
// Состояние
// ============================================================================

static int fft_size = FFT_SIZE;
static int fft_stages = 7;

// ============================================================================
// Внутренние функции
// ============================================================================

// быстрый целочисленный sqrt
static uint16_t isqrt(uint32_t x) {
  uint16_t res = 0;
  uint16_t bit = 1 << 14;

  while (bit > x)
    bit >>= 2;

  while (bit) {
    uint32_t t = res + bit;
    res >>= 1;

    if (x >= t) {
      x -= t;
      res += bit;
    }

    bit >>= 2;
  }

  return res;
}

// 7-битный reverse
static uint8_t bit_reverse7(uint8_t x) {
  x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1);
  x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2);
  x = ((x & 0x0F) << 4) | ((x & 0xF0) >> 4);
  return x >> 1;
}

static void bit_reverse(int16_t *re, int16_t *im) {
  for (int i = 0; i < FFT_SIZE; i++) {
    int j = bit_reverse7(i);

    if (j > i) {
      int16_t tr = re[i];
      re[i] = re[j];
      re[j] = tr;

      tr = im[i];
      im[i] = im[j];
      im[j] = tr;
    }
  }
}

static void fft_butterfly(int16_t *re_ptr, int16_t *im_ptr, int inverse) {
  // Используем int32_t для массивов внутри, чтобы избежать переполнения
  int32_t re[FFT_SIZE];
  int32_t im[FFT_SIZE];
  for (int i = 0; i < FFT_SIZE; i++) {
    re[i] = re_ptr[i];
    im[i] = im_ptr[i];
  }

  int do_scale = !inverse; // Масштабирование только для прямого FFT

  for (int s = 1; s <= fft_stages; s++) {
    int m = 1 << s;
    int mh = m >> 1;
    int step = (FFT_SIZE >> 1) / mh;

    for (int k = 0; k < FFT_SIZE; k += m) {
      for (int j = 0; j < mh; j++) {
        int idx = j * step;

        int32_t wr = cos128[idx];
        int32_t wi = inverse ? sin128[idx] : -sin128[idx];

        int i0 = k + j;
        int i1 = i0 + mh;

        // Округление в Q15-мультипликации (+ (1<<14))
        int32_t tre = (wr * re[i1] - wi * im[i1] + 16384) >> 15;
        int32_t tim = (wr * im[i1] + wi * re[i1] + 16384) >> 15;

        int32_t ure = re[i0];
        int32_t uim = im[i0];

        if (do_scale) {
          // С округлением (+1 перед >>1)
          re[i0] = (ure + tre + 1) >> 1;
          im[i0] = (uim + tim + 1) >> 1;
          re[i1] = (ure - tre + 1) >> 1;
          im[i1] = (uim - tim + 1) >> 1;
        } else {
          // Без масштабирования для IFFT
          re[i0] = ure + tre;
          im[i0] = uim + tim;
          re[i1] = ure - tre;
          im[i1] = uim - tim;
        }
      }
    }
  }

  // Копируем обратно в int16_t (клиппинг, если переполнение, но с правильным
  // input не должно)
  for (int i = 0; i < FFT_SIZE; i++) {
    re_ptr[i] =
        (int16_t)((re[i] > 32767) ? 32767 : (re[i] < -32768 ? -32768 : re[i]));
    im_ptr[i] =
        (int16_t)((im[i] > 32767) ? 32767 : (im[i] < -32768 ? -32768 : im[i]));
  }
}

// ============================================================================
// Публичные функции
// ============================================================================

void FFT_ApplyWindow(int16_t *re) {
  for (int i = 0; i < FFT_SIZE; i++) {
    int32_t v = (int32_t)re[i] * hann128[i];
    re[i] = (int16_t)((v + 16384) >> 15);
  }
}

void FFT_Forward(int16_t *re, int16_t *im) {
  bit_reverse(re, im);
  fft_butterfly(re, im, 0);
}

void FFT_Inverse(int16_t *re, int16_t *im) {
  bit_reverse(re, im);
  fft_butterfly(re, im, 1);
}

void FFT_MagnitudeFast(const int16_t *re, const int16_t *im, uint16_t *mag,
                       int count) {
  for (int k = 0; k < count; k++) {
    int32_t r = re[k];
    int32_t i = im[k];

    if (r < 0)
      r = -r;
    if (i < 0)
      i = -i;

    int32_t mx = (r > i) ? r : i;
    int32_t mn = (r < i) ? r : i;

    int32_t m = mx - (mx >> 5) + ((mn * 13) >> 5);

    mag[k] = (m > 65535) ? 65535 : (uint16_t)m;
  }
}

void FFT_MagnitudeExact(const int16_t *re, const int16_t *im, uint16_t *mag,
                        int count) {
  for (int k = 0; k < count; k++) {
    int32_t r = re[k];
    int32_t i = im[k];

    uint32_t sq = (uint32_t)(r * r) + (uint32_t)(i * i);
    mag[k] = isqrt(sq);
  }
}

int FFT_FindPeak(const uint16_t *mag, int start_bin, int end_bin,
                 uint16_t *out_peak) {
  if (start_bin < 0)
    start_bin = 0;
  if (end_bin >= FFT_SIZE / 2)
    end_bin = FFT_SIZE / 2 - 1;
  if (start_bin > end_bin)
    return -1;

  uint16_t peak_val = mag[start_bin];
  int peak_idx = start_bin;

  for (int i = start_bin + 1; i <= end_bin; i++) {
    if (mag[i] > peak_val) {
      peak_val = mag[i];
      peak_idx = i;
    }
  }

  if (out_peak)
    *out_peak = peak_val;

  return peak_idx;
}

float FFT_BinToFreq(int bin, int sample_rate_hz, int fft_size) {
  return (float)bin * sample_rate_hz / fft_size;
}

void FFT_RemoveDC(int16_t *re) {
  int32_t sum = 0;
  for (int i = 0; i < FFT_SIZE; i++)
    sum += re[i];
  int16_t mean =
      (sum + (FFT_SIZE / 2)) / FFT_SIZE; // или (sum + 64) >> 7 для округления
  for (int i = 0; i < FFT_SIZE; i++)
    re[i] -= mean;
}
