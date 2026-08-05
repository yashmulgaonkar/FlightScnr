#include "hardware/board_marker.h"

#include "hardware/pin_config.h"

/**
 * Searchable identity string baked into every app image.
 *
 * The WebFlasher reads the app partition after connect and looks for the
 * "FSBRDMK:" magic, then takes the following board id. Keep the format and
 * magic in sync with docs/flasher.js (BOARD_MARKER_MAGIC).
 *
 * Referenced from main at boot so --gc-sections cannot drop it.
 */
extern "C" const char kFlightScnrBoardMarker[] =
    "FSBRDMK:" FLIGHTSCNR_BOARD_NAME;
