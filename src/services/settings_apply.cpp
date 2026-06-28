#include "services/settings_apply.h"

#include <cstdlib>

#include "services/adsb_client.h"
#include "services/api_keys.h"
#include "services/map_center.h"
#include "services/weather.h"
#include "services/tz_lookup.h"
#include "hardware/buzzer.h"
#include "hardware/display_brightness.h"
#include "ui/display_prefs.h"
#include "ui/radar_scale.h"

namespace {

SettingsSavedCallback s_saved_callback = nullptr;

}  // namespace

bool settingsApplyFromForm(const char* radar_center_str, const char* lat_str,
                           const char* lon_str, const char* dist_unit_str,
                           const char* legacy_miles_checkbox,
                           const char* cardinals_checkbox,
                           const char* min_height_str, const char* range_mi_str,
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
  if (loc_ok) {
    services::weather::notifyLocationChanged();
    services::tzlookup::notifyLocationChanged();
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

  ui::radar::scaleSaveMilesFromForm(range_mi_str);

  return loc_ok;
}

void settingsSetSavedCallback(SettingsSavedCallback cb) { s_saved_callback = cb; }

void settingsNotifySaved() {
  if (s_saved_callback != nullptr) {
    s_saved_callback();
  }
}
