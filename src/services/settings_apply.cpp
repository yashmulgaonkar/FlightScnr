#include "services/settings_apply.h"

#include <cstdlib>

#include "services/adsb_client.h"
#include "services/api_keys.h"
#include "services/map_center.h"
#include "hardware/buzzer.h"
#include "hardware/display_brightness.h"
#include "ui/display_prefs.h"
#include "ui/radar_scale.h"

namespace {

bool parseRangeIndex(const char* text, uint8_t* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const long v = strtol(text, &end, 10);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  if (v < 0 || v >= static_cast<long>(ui::radar::kScaleBandCount)) {
    return false;
  }
  *out = static_cast<uint8_t>(v);
  return true;
}

}  // namespace

bool settingsApplyFromForm(const char* radar_center_str, const char* lat_str,
                           const char* lon_str, const char* dist_unit_str,
                           const char* legacy_miles_checkbox,
                           const char* cardinals_checkbox,
                           const char* min_height_str, const char* range_index_str,
                           const char* airlabs_key, const char* flightaware_key,
                           const char* fr24_key, const char* use_airlabs_checkbox,
                           const char* use_flightaware_checkbox,
                           const char* use_fr24_checkbox, const char* airlabs_max_calls,
                           const char* flightaware_max_usd, const char* flightaware_cost_usd,
                           const char* fr24_max_usd, const char* fr24_cost_usd,
                           const char* ui_beep_checkbox, const char* beep_tone_str,
                           const char* bright_pct_str, const char* sweep_line_checkbox,
                           const char* detail_timeout_str) {
  bool loc_ok = false;
  if (radar_center_str != nullptr && radar_center_str[0] != '\0') {
    loc_ok = services::map_center::applyRadarCenterFromForm(radar_center_str);
  } else {
    loc_ok = services::map_center::applyPortalCoordinates(lat_str, lon_str);
  }
  ui::radar::saveDistanceUnitsFromForm(dist_unit_str, legacy_miles_checkbox);
  if (cardinals_checkbox != nullptr) {
    ui::radar::saveCompassRoseFromForm(cardinals_checkbox);
  }
  services::adsb::saveAltitudeFloorFromForm(min_height_str);
  services::apikeys::saveFromForm(airlabs_key, flightaware_key, fr24_key);
  services::apikeys::saveEnabledFromForm(use_airlabs_checkbox, use_flightaware_checkbox,
                                         use_fr24_checkbox);
  services::apikeys::saveLimitsFromForm(airlabs_max_calls, flightaware_max_usd,
                                        flightaware_cost_usd, fr24_max_usd, fr24_cost_usd);
  hardware::saveBeepEnabledFromForm(ui_beep_checkbox);
  hardware::saveBeepToneFromForm(beep_tone_str);
  hardware::displayBrightnessSaveFromForm(bright_pct_str);
  ui::displayPrefsSaveSweepLineFromForm(sweep_line_checkbox);
  ui::displayPrefsSaveFlightDetailTimeoutFromForm(detail_timeout_str);

  uint8_t range_idx = 0;
  if (parseRangeIndex(range_index_str, &range_idx)) {
    ui::radar::scaleSelect(range_idx);
  }

  return loc_ok;
}
