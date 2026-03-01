#pragma once
#include <stdint.h>

#define FFT_SIZE 128

void FFT_Forward(int16_t *re, int16_t *im);
void FFT_Inverse(int16_t *re, int16_t *im);

// Магнитуды
void FFT_MagnitudeFast(const int16_t *re, const int16_t *im, uint16_t *mag,
                       int count);
void FFT_MagnitudeExact(const int16_t *re, const int16_t *im, uint16_t *mag,
                        int count);

// Утилиты
int FFT_FindPeak(const uint16_t *mag, int start_bin, int end_bin,
                 uint16_t *out_peak);
float FFT_BinToFreq(int bin, int sample_rate_hz, int fft_size);
void FFT_LogScale(const uint16_t *mag_in, uint8_t *mag_out, int bins_in,
                  int bins_out, uint8_t min_db);
void FFT_ApplyWindow(int16_t *re);
void FFT_RemoveDC(int16_t *re);
