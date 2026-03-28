#include "bands.h"
#include "../driver/bk4829.h"
#include "../driver/lfs.h"
#include "../driver/uart.h"
#include "../misc.h"
#include "../radio.h"
#include "measurements.h"
#include "storage.h"

const Band DEFAULT_BAND = {
    .name = "Unknown",
    .step = STEP_25_0kHz,
    .bw = BK4819_FILTER_BW_12k,
    .start = 0,
    .end = 1340 * MHZ,
    .squelch.value = 4,
};

Band defaultBands[] = {
    (Band){
        .start = 5000,
        .end = 13569,
        .name = "VLF/LF",
        .step = STEP_0_05kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = false,
        .lastUsedFreq = 10000,
    },
    (Band){
        .start = 13570,
        .end = 13779,
        .name = "LW HAM",
        .step = STEP_0_05kHz,
        .modulation = MOD_LSB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 13570,
    },
    (Band){
        .start = 14850,
        .end = 28349,
        .name = "LW",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 14850,
    },
    (Band){
        .start = 15000,
        .end = 26964,
        .name = "Military1",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = false,
        .lastUsedFreq = 15000,
    },
    (Band){
        .start = 52650,
        .end = 160649,
        .name = "MW",
        .step = STEP_9_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 52650,
    },
    (Band){
        .start = 181000,
        .end = 199999,
        .name = "160m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_LSB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 181000,
    },
    (Band){
        .start = 230000,
        .end = 249499,
        .name = "120m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 230000,
    },
    (Band){
        .start = 250000,
        .end = 318999,
        .name = "HULIGAN",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 250000,
    },
    (Band){
        .start = 320000,
        .end = 339999,
        .name = "90m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 320000,
    },
    (Band){
        .start = 350000,
        .end = 379999,
        .name = "80m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_LSB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 350000,
    },
    (Band){
        .start = 380000,
        .end = 389999,
        .name = "75m AM",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 385000,
    },
    (Band){
        .start = 390000,
        .end = 399999,
        .name = "75m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 390000,
    },
    (Band){
        .start = 475000,
        .end = 505999,
        .name = "60m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 475000,
    },
    (Band){
        .start = 525850,
        .end = 540649,
        .name = "60m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_LSB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 525850,
    },
    (Band){
        .start = 585000,
        .end = 634999,
        .name = "49m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 585000,
    },
    (Band){
        .start = 700000,
        .end = 719899,
        .name = "40m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_LSB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 700000,
    },
    (Band){
        .start = 720000,
        .end = 749999,
        .name = "41m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 720000,
    },
    (Band){
        .start = 940000,
        .end = 998999,
        .name = "31m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 940000,
    },
    (Band){
        .start = 1010000,
        .end = 1014999,
        .name = "30m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_USB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 1010000,
    },
    (Band){
        .start = 1160000,
        .end = 1209999,
        .name = "25m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 1160000,
    },
    (Band){
        .start = 1350000,
        .end = 1379999,
        .name = "22m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 1350000,
    },
    (Band){
        .start = 1400000,
        .end = 1434999,
        .name = "20m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_USB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 1400000,
    },
    (Band){
        .start = 1510000,
        .end = 1559999,
        .name = "19m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 1510000,
    },
    (Band){
        .start = 1755000,
        .end = 1804999,
        .name = "16m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 1755000,
    },
    (Band){
        .start = 1806800,
        .end = 1816799,
        .name = "17m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_USB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 1806800,
    },
    (Band){
        .start = 1890000,
        .end = 1901999,
        .name = "15m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 1890000,
    },
    (Band){
        .start = 2100000,
        .end = 2144999,
        .name = "15m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_USB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 2100000,
    },
    (Band){
        .start = 2145000,
        .end = 2184999,
        .name = "13m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 2145000,
    },
    (Band){
        .start = 2400000,
        .end = 2799999,
        .name = "11m CB",
        .step = STEP_10_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 2720000,
    },
    (Band){
        .start = 2489000,
        .end = 2498999,
        .name = "12m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_USB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 2489000,
    },
    (Band){
        .start = 2560000,
        .end = 2609999,
        .name = "11m",
        .step = STEP_5_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 2560000,
    },
    (Band){
        .start = 2696500,
        .end = 2760123,
        .name = "CB EU",
        .step = STEP_10_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_9k,
        .allowTx = true,
        .lastUsedFreq = 2696500,
    },
    (Band){
        .start = 2760125,
        .end = 2799124,
        .name = "CB UK",
        .step = STEP_10_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 2760125,
    },
    (Band){
        .start = 2799130,
        .end = 6399998,
        .name = "Military2",
        .step = STEP_12_5kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 2799130,
    },
    (Band){
        .start = 6400000,
        .end = 8799998,
        .name = "Military3",
        .step = STEP_12_5kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 6400000,
    },
    (Band){
        .start = 8800000,
        .end = 10799998,
        .name = "FM",
        .step = STEP_100_0kHz,
        .modulation = MOD_WFM,
        .bw = BK4819_FILTER_BW_26k,
        .allowTx = true,
        .lastUsedFreq = 8800000,
    },
    (Band){
        .start = 10800000,
        .end = 11799998,
        .name = "108-118",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 10800000,
    },
    (Band){
        .start = 11800000,
        .end = 13694999,
        .name = "Air",
        .step = STEP_8_33kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 11800000,
    },
    (Band){
        .start = 13700000,
        .end = 14397499,
        .name = "MeteoKosm",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 13700000,
    },
    (Band){
        .start = 14400000,
        .end = 14599999,
        .name = "2m HAM",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 14400000,
    },
    (Band){
        .start = 14600000,
        .end = 14799999,
        .name = "2m HAM E",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 14600000,
    },
    (Band){
        .start = 14800000,
        .end = 15549998,
        .name = "MiRGD",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 14800000,
    },
    (Band){
        .start = 15550000,
        .end = 16199998,
        .name = "Marine",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 15550000,
    },
    (Band){
        .start = 16200000,
        .end = 17399998,
        .name = "Business3",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 16200000,
    },
    (Band){
        .start = 17400000,
        .end = 24499998,
        .name = "MSatcom1",
        .step = STEP_25_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 17400000,
    },
    (Band){
        .start = 24500000,
        .end = 26999998,
        .name = "MSatcom2",
        .step = STEP_25_0kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 24500000,
    },
    (Band){
        .start = 27000000,
        .end = 42999998,
        .name = "Military4",
        .step = STEP_12_5kHz,
        .modulation = MOD_AM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 27000000,
    },
    (Band){
        .start = 43000000,
        .end = 43307498,
        .name = "70cmHAM1",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 43000000,
    },
    (Band){
        .start = 43307500,
        .end = 43477499,
        .name = "LPD",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 43307500,
    },
    (Band){
        .start = 43480000,
        .end = 43999998,
        .name = "70cmHAM2",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 43480000,
    },
    (Band){
        .start = 44000000,
        .end = 44600623,
        .name = "Business4",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 44000000,
    },
    (Band){
        .start = 44600625,
        .end = 44619374,
        .name = "PMR",
        .step = STEP_6_25kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 44600625,
    },
    (Band){
        .start = 44620000,
        .end = 46262489,
        .name = "Business5",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 44620000,
    },
    (Band){
        .start = 46256250,
        .end = 46273748,
        .name = "FRS/G462",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 46256250,
    },
    (Band){
        .start = 46273750,
        .end = 46756248,
        .name = "Business6",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 46273750,
    },
    (Band){
        .start = 46756250,
        .end = 46774998,
        .name = "FRS/G467",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 46756250,
    },
    (Band){
        .start = 46775000,
        .end = 46999998,
        .name = "Business7",
        .step = STEP_12_5kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 46775000,
    },
    (Band){
        .start = 47000000,
        .end = 61999998,
        .name = "470-620",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 47000000,
    },
    (Band){
        .start = 62000000,
        .end = 83999998,
        .name = "UHF TV",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_26k,
        .allowTx = false,
        .lastUsedFreq = 62000000,
    },
    (Band){
        .start = 84000000,
        .end = 86299998,
        .name = "840-863",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 84000000,
    },
    (Band){
        .start = 86300000,
        .end = 86300000,
        .name = "LORA",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 86300000,
    },
    (Band){
        .start = 87000000,
        .end = 88999998,
        .name = "870-890",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 87000000,
    },
    (Band){
        .start = 89000000,
        .end = 95999998,
        .name = "GSM-900",
        .step = STEP_100_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_26k,
        .allowTx = true,
        .lastUsedFreq = 89000000,
    },
    (Band){
        .start = 96000000,
        .end = 125999998,
        .name = "960-1260",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 96000000,
    },
    (Band){
        .start = 126000000,
        .end = 129999998,
        .name = "23cm HAM",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 126000000,
    },
    (Band){
        .start = 130000000,
        .end = 133899999,
        .name = "1.3-1.34",
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
        .allowTx = true,
        .lastUsedFreq = 130000000,
    },
    (Band){
        .start = 28000000,
        .end = 29699999,
        .name = "10m HAM",
        .step = STEP_1_0kHz,
        .modulation = MOD_USB,
        .bw = BK4819_FILTER_BW_6k,
        .allowTx = true,
        .lastUsedFreq = 28000000,
    },
};

static const PowerCalibration DEFAULT_POWER_CALIB = {43, 68, 140};

PCal POWER_CALIBRATIONS[] = {
    {.s = 135 * MHZ, .e = 165 * MHZ, .c = {38, 65, 140}},
    {.s = 165 * MHZ, .e = 205 * MHZ, .c = {36, 52, 140}},
    {.s = 205 * MHZ, .e = 215 * MHZ, .c = {41, 64, 135}},
    {.s = 215 * MHZ, .e = 220 * MHZ, .c = {44, 46, 50}},
    {.s = 220 * MHZ, .e = 240 * MHZ, .c = {0, 0, 0}},
    {.s = 240 * MHZ, .e = 265 * MHZ, .c = {62, 82, 130}},
    {.s = 265 * MHZ, .e = 270 * MHZ, .c = {65, 92, 140}},
    {.s = 270 * MHZ, .e = 275 * MHZ, .c = {73, 103, 140}},
    {.s = 275 * MHZ, .e = 285 * MHZ, .c = {81, 107, 140}},
    {.s = 285 * MHZ, .e = 295 * MHZ, .c = {57, 94, 140}},
    {.s = 295 * MHZ, .e = 305 * MHZ, .c = {74, 104, 140}},
    {.s = 305 * MHZ, .e = 335 * MHZ, .c = {81, 107, 140}},
    {.s = 335 * MHZ, .e = 345 * MHZ, .c = {63, 98, 140}},
    {.s = 345 * MHZ, .e = 355 * MHZ, .c = {52, 89, 140}},
    {.s = 355 * MHZ, .e = 365 * MHZ, .c = {46, 74, 140}},
    {.s = 470 * MHZ, .e = 620 * MHZ, .c = {46, 77, 140}},
};

static Band rangesStack[RANGES_STACK_SIZE] = {0};
static int8_t rangesStackIndex = -1;

void BANDS_Recreate() {
  for (uint8_t i = 0; i < 67; ++i) {
    STORAGE_SAVE("Bands.bnd", i, &defaultBands[i]);
  }
}

bool BANDS_InRange(uint32_t f, Band *b) { return b->start <= f && f < b->end; }

Band BANDS_ByFrequency(uint32_t f) {
  // Band b[MAX_BANDS];
  // Storage_LoadMultiple("Bands.bnd", 0, b, sizeof(Band), MAX_BANDS);
  for (uint8_t i = 0; i < MAX_BANDS; ++i) {
    if (IsReadable(defaultBands[i].name)) {
      if (BANDS_InRange(f, &defaultBands[i])) {
        return defaultBands[i];
      }
    }
  }
  return DEFAULT_BAND;
}

PowerCalibration BANDS_GetPowerCalib(uint32_t f) {
  Band b = BANDS_ByFrequency(f);

  // TODO: not TYPE_BAND_DETACHED
  if (b.powCalib.e > 0) {
    return b.powCalib;
  }

  for (uint8_t ci = 0; ci < ARRAY_SIZE(POWER_CALIBRATIONS); ++ci) {
    PCal cal = POWER_CALIBRATIONS[ci];
    if (cal.s <= f && f < cal.e) {
      return cal.c;
    }
  }

  return DEFAULT_POWER_CALIB;
}

uint8_t BANDS_CalculateOutputPower(TXOutputPower power, uint32_t f) {
  uint8_t power_bias;
  PowerCalibration cal = BANDS_GetPowerCalib(f);

  switch (power) {
  case TX_POW_LOW:
    power_bias = cal.s;
    break;

  case TX_POW_MID:
    power_bias = cal.m;
    break;

  case TX_POW_HIGH:
    power_bias = cal.e;
    break;

  default:
    power_bias = cal.s;
    if (power_bias > 10)
      power_bias -= 10; // 10mw if Low=500mw
  }

  return power_bias;
}

void BANDS_RangeClear() { rangesStackIndex = -1; }
int8_t BANDS_RangeIndex() { return rangesStackIndex; }

bool BANDS_RangePush(Band r) {
  if (rangesStackIndex < RANGES_STACK_SIZE - 1) {
    // Log("range +");
    rangesStack[++rangesStackIndex] = r;
  }
  return true;
}

Band BANDS_RangePop(void) {
  if (rangesStackIndex > 0) {
    // Log("range -");
    return rangesStack[rangesStackIndex--];
  }
  return rangesStack[rangesStackIndex];
}

Band *BANDS_RangePeek(void) {
  if (rangesStackIndex >= 0) {
    // Log("range peek ok");
    return &rangesStack[rangesStackIndex];
  }
  return NULL;
}

uint16_t BANDS_GetStepSize(Band *p) { return StepFrequencyTable[p->step]; }

uint32_t BANDS_GetSteps(Band *p) {
  return (p->end - p->start) / BANDS_GetStepSize(p) + 1;
}

uint32_t BANDS_GetF(Band *p, uint32_t channel) {
  return p->start + channel * BANDS_GetStepSize(p);
}

uint32_t BANDS_GetChannel(Band *p, uint32_t f) {
  return (f - p->start) / BANDS_GetStepSize(p);
}
