#pragma once

namespace services::adsb {

/**
 * Decide whether an aircraft's altitude falls within the configured
 * min-max altitude band.
 *
 * Pure, hardware-free transformation (test seam). No Arduino/ESP/LVGL deps.
 *
 * A limit of <= 0 disables that edge of the band:
 *   - Both limits off                          -> always true.
 *   - Any limit active and !has_altitude       -> false (unknown excluded).
 *   - Floor active and alt_ft < floor_ft       -> false.
 *   - Ceiling active and alt_ft > ceiling_ft   -> false.
 *   - Otherwise                                -> true.
 *
 * Boundaries are inclusive. No cross-validation: floor > ceiling yields an
 * empty band, so nothing passes.
 */
bool altitudeWithinBand(bool has_altitude, float alt_ft, int floor_ft,
                        int ceiling_ft);

}  // namespace services::adsb
