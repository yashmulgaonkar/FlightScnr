#include "services/altitude_filter.h"

namespace services::adsb {

bool altitudeWithinBand(bool has_altitude, float alt_ft, int floor_ft,
                        int ceiling_ft) {
  const bool floor_active = floor_ft > 0;
  const bool ceiling_active = ceiling_ft > 0;
  if (!floor_active && !ceiling_active) {
    return true;
  }
  if (!has_altitude) {
    return false;
  }
  if (floor_active && alt_ft < static_cast<float>(floor_ft)) {
    return false;
  }
  if (ceiling_active && alt_ft > static_cast<float>(ceiling_ft)) {
    return false;
  }
  return true;
}

}  // namespace services::adsb
