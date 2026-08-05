#include "ui/radar_scale.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "services/settings_state.h"

namespace ui::radar {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kRangeMiKey[] = "range_mi";
constexpr char kDistUnitKey[] = "dist_unit";
constexpr char kDistMiKey[] = "dist_mi";
constexpr char kRoseKey[] = "rose_en";
constexpr char kFacingKey[] = "facing_deg";

constexpr char kLegacyScaleKey[] = "rangeIdx";
constexpr char kLegacyScaleSlotKey[] = "scale_slot";
constexpr char kLegacyMilesKey[] = "useMiles";
constexpr char kLegacyRoseKey[] = "showCard";

constexpr uint8_t kDefaultRangeMiles = 8;
constexpr uint8_t kLegacyMilesFromIndex[] = {2, 6, 6, 8};
constexpr uint16_t kFacingStepDeg = 5;

uint8_t s_active_miles = kDefaultRangeMiles;
ScaleBand s_active_band{};
DistanceUnit s_distance_unit = DistanceUnit::Km;
bool s_compass_rose = true;
uint16_t s_facing_deg = 0;

uint16_t normalizeFacingDeg(int deg) {
  int d = deg % 360;
  if (d < 0) {
    d += 360;
  }
  // Snap to nearest 5° step.
  d = ((d + static_cast<int>(kFacingStepDeg) / 2) / static_cast<int>(kFacingStepDeg)) *
      static_cast<int>(kFacingStepDeg);
  if (d >= 360) {
    d = 0;
  }
  return static_cast<uint16_t>(d);
}

bool formCheckboxOn(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  if ((value[0] == 'F' || value[0] == 'f') && value[1] == '\0') {
    return false;
  }
  if ((value[0] == 'T' || value[0] == 't') && value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

DistanceUnit parseDistanceUnit(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return DistanceUnit::Km;
  }
  if (strcmp(value, "mi") == 0 || strcmp(value, "miles") == 0) {
    return DistanceUnit::StatuteMile;
  }
  if (strcmp(value, "nm") == 0) {
    return DistanceUnit::NauticalMile;
  }
  return DistanceUnit::Km;
}

void persistU8(const char* key, uint8_t v) {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(key, v);
    prefs.end();
  }
}

void persistDistanceUnit(DistanceUnit unit) {
  persistU8(kDistUnitKey, static_cast<uint8_t>(unit));
  settingsStateBump();
}

bool isAllowedMile(uint8_t miles) {
  for (size_t i = 0; i < kRangeMileOptionCount; ++i) {
    if (kRangeMileOptions[i] == miles) {
      return true;
    }
  }
  return false;
}

int optionIndexForMiles(uint8_t miles) {
  for (size_t i = 0; i < kRangeMileOptionCount; ++i) {
    if (kRangeMileOptions[i] == miles) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void recomputeActiveBand() {
  s_active_band.label_km = static_cast<float>(s_active_miles) * kStatuteMileKm;
  s_active_band.coverage_km = s_active_band.label_km * kLabelToCoverageKm;
}

void applyMiles(uint8_t miles) {
  if (!isAllowedMile(miles)) {
    return;
  }
  s_active_miles = miles;
  recomputeActiveBand();
  persistU8(kRangeMiKey, s_active_miles);
  settingsStateBump();
}

uint8_t migrateLegacyRangeIndex(uint8_t legacy_index) {
  if (legacy_index < sizeof(kLegacyMilesFromIndex)) {
    return kLegacyMilesFromIndex[legacy_index];
  }
  return kDefaultRangeMiles;
}

}  // namespace

void scaleBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    recomputeActiveBand();
    return;
  }

  if (prefs.isKey(kRangeMiKey)) {
    const uint8_t stored = prefs.getUChar(kRangeMiKey, kDefaultRangeMiles);
    if (isAllowedMile(stored)) {
      s_active_miles = stored;
    } else if (stored > 30) {
      s_active_miles = 30;
    } else {
      s_active_miles = kDefaultRangeMiles;
    }
  } else {
    uint8_t legacy_index = prefs.getUChar(kLegacyScaleSlotKey, 255);
    if (legacy_index == 255) {
      legacy_index = prefs.getUChar(kLegacyScaleKey, 1);
    }
    s_active_miles = migrateLegacyRangeIndex(legacy_index);
  }

  if (prefs.isKey(kDistUnitKey)) {
    const uint8_t raw = prefs.getUChar(kDistUnitKey, 0);
    s_distance_unit = (raw <= static_cast<uint8_t>(DistanceUnit::NauticalMile))
                          ? static_cast<DistanceUnit>(raw)
                          : DistanceUnit::Km;
  } else if (prefs.isKey(kDistMiKey)) {
    s_distance_unit =
        prefs.getBool(kDistMiKey, false) ? DistanceUnit::StatuteMile : DistanceUnit::Km;
  } else {
    s_distance_unit =
        prefs.getBool(kLegacyMilesKey, false) ? DistanceUnit::StatuteMile : DistanceUnit::Km;
  }

  if (prefs.isKey(kRoseKey)) {
    s_compass_rose = prefs.getBool(kRoseKey, true);
  } else {
    s_compass_rose = prefs.getBool(kLegacyRoseKey, true);
  }

  s_facing_deg = normalizeFacingDeg(static_cast<int>(prefs.getUShort(kFacingKey, 0)));

  prefs.end();
  recomputeActiveBand();
}

void scaleIncrease() { scaleStep(1); }

void scaleDecrease() { scaleStep(-1); }

void scaleStep(int8_t delta) {
  if (delta == 0) {
    return;
  }
  int idx = optionIndexForMiles(s_active_miles);
  if (idx < 0) {
    idx = 0;
  }
  if (delta > 0) {
    idx = static_cast<int>((static_cast<size_t>(idx) + 1) % kRangeMileOptionCount);
  } else {
    idx = (idx == 0) ? static_cast<int>(kRangeMileOptionCount - 1) : idx - 1;
  }
  applyMiles(kRangeMileOptions[static_cast<size_t>(idx)]);
}

void scaleSelect(uint8_t option_index) {
  if (option_index >= kRangeMileOptionCount) {
    return;
  }
  applyMiles(kRangeMileOptions[option_index]);
}

bool scaleSetMiles(uint8_t miles) {
  if (!isAllowedMile(miles)) {
    return false;
  }
  applyMiles(miles);
  return true;
}

bool scaleSaveMilesFromForm(const char* range_str) {
  if (range_str == nullptr || range_str[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const long value = strtol(range_str, &end, 10);
  if (end == range_str || value <= 0 || value > 255) {
    return false;
  }
  const uint8_t miles = static_cast<uint8_t>(value);
  if (optionIndexForMiles(miles) < 0) {
    return false;
  }
  applyMiles(miles);
  Serial.printf("Range: %u mi\n", static_cast<unsigned>(s_active_miles));
  return true;
}

const ScaleBand& scaleActive() { return s_active_band; }

uint8_t scaleActiveIndex() {
  const int idx = optionIndexForMiles(s_active_miles);
  return idx >= 0 ? static_cast<uint8_t>(idx) : 0;
}

uint8_t scaleActiveMiles() { return s_active_miles; }

float adsbQueryRadiusKm() {
  const float coverage_km = s_active_band.coverage_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return coverage_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

DistanceUnit distanceUnit() { return s_distance_unit; }

const char* distanceUnitLabel() {
  switch (s_distance_unit) {
    case DistanceUnit::StatuteMile:
      return "miles";
    case DistanceUnit::NauticalMile:
      return "nm";
    default:
      return "km";
  }
}

void cycleDistanceUnits() {
  const uint8_t next =
      (static_cast<uint8_t>(s_distance_unit) + 1) %
      (static_cast<uint8_t>(DistanceUnit::NauticalMile) + 1);
  s_distance_unit = static_cast<DistanceUnit>(next);
  persistDistanceUnit(s_distance_unit);
  Serial.printf("Distance units: %s\n", distanceUnitLabel());
}

void saveDistanceUnitsFromForm(const char* unit_value,
                               const char* legacy_miles_checkbox) {
  if (unit_value != nullptr && unit_value[0] != '\0') {
    s_distance_unit = parseDistanceUnit(unit_value);
  } else {
    s_distance_unit = formCheckboxOn(legacy_miles_checkbox) ? DistanceUnit::StatuteMile
                                                            : DistanceUnit::Km;
  }
  persistDistanceUnit(s_distance_unit);
  Serial.printf("Distance units: %s\n", distanceUnitLabel());
}

bool showCompassRose() { return s_compass_rose; }

void toggleCompassRose() {
  s_compass_rose = !s_compass_rose;
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kRoseKey, s_compass_rose);
    prefs.end();
  }
  settingsStateBump();
  Serial.printf("Compass rose: %s\n", s_compass_rose ? "on" : "off");
}

void saveCompassRoseFromForm(const char* checkbox_value) {
  s_compass_rose = formCheckboxOn(checkbox_value);
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kRoseKey, s_compass_rose);
    prefs.end();
  }
  settingsStateBump();
  Serial.printf("Compass rose: %s\n", s_compass_rose ? "on" : "off");
}

uint16_t facingDeg() { return s_facing_deg; }

void applyFacingDeg(uint16_t deg) { s_facing_deg = normalizeFacingDeg(static_cast<int>(deg)); }

void facingStep(int8_t delta) {
  if (delta == 0) {
    return;
  }
  // Negate so dial CW decreases facing (radar rose turns with the dial).
  // Keep the sum signed through normalize — casting a negative angle to
  // uint16_t wraps (e.g. -5 → 65531 → snaps to 10°) and stalls CW at 0–10°.
  const int step = -static_cast<int>(delta) * static_cast<int>(kFacingStepDeg);
  s_facing_deg = normalizeFacingDeg(static_cast<int>(s_facing_deg) + step);
}

void persistFacingDeg() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUShort(kFacingKey, s_facing_deg);
    prefs.end();
  }
  settingsStateBump();
  Serial.printf("Radar facing: %u deg\n", static_cast<unsigned>(s_facing_deg));
}

void setFacingDeg(uint16_t deg) {
  applyFacingDeg(deg);
  persistFacingDeg();
}

void saveFacingDegFromForm(const char* degrees_str) {
  if (degrees_str == nullptr || degrees_str[0] == '\0') {
    return;
  }
  char* end = nullptr;
  const long v = strtol(degrees_str, &end, 10);
  if (end == degrees_str) {
    return;
  }
  setFacingDeg(static_cast<uint16_t>(v));
}

void facingLabel(char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  switch (s_facing_deg) {
    case 0:
      snprintf(out, out_len, "N");
      break;
    case 90:
      snprintf(out, out_len, "E");
      break;
    case 180:
      snprintf(out, out_len, "S");
      break;
    case 270:
      snprintf(out, out_len, "W");
      break;
    default:
      snprintf(out, out_len, "%u°", static_cast<unsigned>(s_facing_deg));
      break;
  }
}

void formatScaleTag(char* buf, size_t len, float label_km, DistanceUnit unit) {
  float value = label_km;
  const char* suffix = "km";
  switch (unit) {
    case DistanceUnit::StatuteMile:
      value = label_km / kStatuteMileKm;
      suffix = "mi";
      break;
    case DistanceUnit::NauticalMile:
      value = label_km / kNauticalMileKm;
      suffix = "nm";
      break;
    default:
      break;
  }

  // Integer rounding collapses nearby rings on small ranges (2mi / 3 rings →
  // 0.67 and 1.33 both became "1mi"). Keep a tenth when not a whole number.
  const float nearest = roundf(value);
  if (fabsf(value - nearest) < 0.05f) {
    snprintf(buf, len, "%d%s", static_cast<int>(nearest), suffix);
  } else {
    snprintf(buf, len, "%.1f%s", static_cast<double>(value), suffix);
  }
}

void formatActiveScaleTag(char* buf, size_t len) {
  formatScaleTag(buf, len, s_active_band.label_km, s_distance_unit);
}

void formatAltitudeDisplay(const char* alt_ft_tag, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (alt_ft_tag == nullptr || alt_ft_tag[0] == '\0') {
    return;
  }
  if (strcmp(alt_ft_tag, "GND") == 0) {
    strncpy(out, "GND", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }

  int ft = 0;
  if (sscanf(alt_ft_tag, "%d ft", &ft) != 1) {
    strncpy(out, alt_ft_tag, out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }

  if (s_distance_unit == DistanceUnit::Km) {
    snprintf(out, out_len, "%d m", static_cast<int>(lroundf(ft * kFeetToMeters)));
  } else {
    snprintf(out, out_len, "%d ft", ft);
  }
}

void formatSpeedLabel(char* out, size_t out_len, float gs_knots) {
  if (gs_knots <= 0.5f) {
    strncpy(out, "Speed: —", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }

  switch (s_distance_unit) {
    case DistanceUnit::Km:
      snprintf(out, out_len, "Speed: %d km/h",
               static_cast<int>(lroundf(gs_knots * kKnotsToKmh)));
      break;
    case DistanceUnit::StatuteMile:
      snprintf(out, out_len, "Speed: %d mph",
               static_cast<int>(lroundf(gs_knots * kKnotsToMph)));
      break;
    default:
      snprintf(out, out_len, "Speed: %d kt", static_cast<int>(lroundf(gs_knots)));
      break;
  }
}

}  // namespace ui::radar
