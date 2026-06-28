#include "ui/radar_scale.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kRangeMiKey[] = "range_mi";
constexpr char kDistUnitKey[] = "dist_unit";
constexpr char kDistMiKey[] = "dist_mi";
constexpr char kRoseKey[] = "rose_en";

constexpr char kLegacyScaleKey[] = "rangeIdx";
constexpr char kLegacyScaleSlotKey[] = "scale_slot";
constexpr char kLegacyMilesKey[] = "useMiles";
constexpr char kLegacyRoseKey[] = "showCard";

constexpr uint8_t kDefaultRangeMiles = 8;
constexpr uint8_t kLegacyMilesFromIndex[] = {2, 6, 6, 8};

uint8_t s_active_miles = kDefaultRangeMiles;
ScaleBand s_active_band{};
DistanceUnit s_distance_unit = DistanceUnit::Km;
bool s_compass_rose = true;

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

namespace {

uint8_t snapMilesToPreset(float miles) {
  uint8_t best = kRangeMileOptions[0];
  float best_diff = 1e9f;
  for (size_t i = 0; i < kRangeMileOptionCount; ++i) {
    const float diff = fabsf(static_cast<float>(kRangeMileOptions[i]) - miles);
    if (diff < best_diff) {
      best_diff = diff;
      best = kRangeMileOptions[i];
    }
  }
  return best;
}

}  // namespace

bool scaleSaveMilesFromForm(const char* range_str) {
  if (range_str == nullptr || range_str[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const float value = strtof(range_str, &end);
  if (end == range_str || value <= 0.0f) {
    return false;
  }

  // Optional unit suffix: km / nm / mi (default statute miles).
  while (*end == ' ') {
    ++end;
  }
  float miles = value;
  if (end[0] != '\0') {
    if ((end[0] == 'k' || end[0] == 'K') && (end[1] == 'm' || end[1] == 'M')) {
      miles = value / kStatuteMileKm;
    } else if ((end[0] == 'n' || end[0] == 'N') && (end[1] == 'm' || end[1] == 'M')) {
      miles = value * (kNauticalMileKm / kStatuteMileKm);
    } else if ((end[0] == 'm' || end[0] == 'M') && (end[1] == 'i' || end[1] == 'I')) {
      miles = value;
    } else {
      return false;  // Unrecognized unit.
    }
  }

  if (miles < 1.0f || miles > 60.0f) {
    return false;
  }
  const uint8_t snapped = snapMilesToPreset(miles);
  applyMiles(snapped);
  Serial.printf("Range: %u mi (from \"%s\")\n", static_cast<unsigned>(s_active_miles),
                range_str);
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
  Serial.printf("Compass rose: %s\n", s_compass_rose ? "on" : "off");
}

void saveCompassRoseFromForm(const char* checkbox_value) {
  s_compass_rose = formCheckboxOn(checkbox_value);
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kRoseKey, s_compass_rose);
    prefs.end();
  }
  Serial.printf("Compass rose: %s\n", s_compass_rose ? "on" : "off");
}

void formatScaleTag(char* buf, size_t len, float label_km, DistanceUnit unit) {
  switch (unit) {
    case DistanceUnit::StatuteMile: {
      const int mi = static_cast<int>(lroundf(label_km / kStatuteMileKm));
      snprintf(buf, len, "%dmi", mi);
      break;
    }
    case DistanceUnit::NauticalMile: {
      const int nm = static_cast<int>(lroundf(label_km / kNauticalMileKm));
      snprintf(buf, len, "%dnm", nm);
      break;
    }
    default: {
      const int km = static_cast<int>(lroundf(label_km));
      snprintf(buf, len, "%dkm", km);
      break;
    }
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
