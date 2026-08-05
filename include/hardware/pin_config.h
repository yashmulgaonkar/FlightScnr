#pragma once

/**
 * Compile-time board pin map.
 *
 * Selected by PlatformIO build flag:
 *   -D FLIGHTSCNR_BOARD_TENCODER_PRO   (LilyGO T-Encoder Pro — default)
 *   -D FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18  (Waveshare ESP32-S3-Knob-Touch-LCD-1.8)
 *
 * Run `pio run` (env: auto) to pick a connected board, or `pio run -e <board>`.
 */

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
#include "boards/waveshare_knob_18.h"
#elif defined(FLIGHTSCNR_BOARD_TENCODER_PRO)
#include "boards/tencoder_pro.h"
#else
/* Native/host unit tests and accidental bare builds default to T-Encoder Pro. */
#include "boards/tencoder_pro.h"
#endif
