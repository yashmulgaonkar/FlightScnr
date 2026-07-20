#pragma once

/**
 * Apply portal/web form fields and persist to NVS.
 * Web: pass radar_center as "lat, lon". Leave lat_str/lon_str null.
 * Captive portal (4.3.2.1) is Wi‑Fi only and does not call this function.
 * Returns false if coordinates invalid.
 */
bool settingsApplyFromForm(const char* radar_center_str, const char* lat_str,
                           const char* lon_str, const char* dist_unit_str,
                           const char* legacy_miles_checkbox,
                           const char* cardinals_checkbox,
                           const char* min_height_str, const char* max_height_str,
                           const char* range_mi_str,
                           const char* airlabs_key, const char* flightaware_key,
                           const char* fr24_key, const char* use_airlabs_checkbox,
                           const char* use_flightaware_checkbox,
                           const char* use_fr24_checkbox, const char* airlabs_max_calls,
                           const char* flightaware_max_usd, const char* flightaware_cost_usd,
                           const char* fr24_max_usd, const char* fr24_cost_usd,
                           const char* ui_beep_checkbox, const char* beep_tone_str,
                           const char* bright_pct_str, const char* sweep_line_checkbox,
                           const char* detail_timeout_str);

using SettingsSavedCallback = void (*)();
void settingsSetSavedCallback(SettingsSavedCallback cb);
void settingsNotifySaved();
