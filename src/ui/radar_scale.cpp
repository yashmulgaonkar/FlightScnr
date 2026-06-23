#include "ui/radar_scale.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kScaleSlotKey[] = "scale_slot";
constexpr char kDistUnitKey[] = "dist_unit";
constexpr char kDistMiKey[] = "dist_mi";
constexpr char kRoseKey[] = "rose_en";

constexpr char kLegacyScaleKey[] = "rangeIdx";
constexpr char kLegacyMilesKey[] = "useMiles";
constexpr char kLegacyRoseKey[] = "showCard";

constexpr uint8_t kDefaultScaleIndex = 1;

uint8_t s_active_index = kDefaultScaleIndex;
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

}  // namespace

void scaleBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }

  uint8_t slot = prefs.getUChar(kScaleSlotKey, 255);
  if (slot == 255) {
    slot = prefs.getUChar(kLegacyScaleKey, kDefaultScaleIndex);
  }
  s_active_index = (slot < kScaleBandCount) ? slot : kDefaultScaleIndex;

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
}

void scaleIncrease() {
  s_active_index = static_cast<uint8_t>((s_active_index + 1) % kScaleBandCount);
  persistU8(kScaleSlotKey, s_active_index);
}

void scaleDecrease() {
  s_active_index = (s_active_index == 0)
                       ? static_cast<uint8_t>(kScaleBandCount - 1)
                       : static_cast<uint8_t>(s_active_index - 1);
  persistU8(kScaleSlotKey, s_active_index);
}

void scaleSelect(uint8_t index) {
  if (index >= kScaleBandCount) {
    return;
  }
  s_active_index = index;
  persistU8(kScaleSlotKey, s_active_index);
}

const ScaleBand& scaleActive() { return kScaleBands[s_active_index]; }

uint8_t scaleActiveIndex() { return s_active_index; }

float adsbQueryRadiusKm() {
  const float coverage_km = scaleActive().coverage_km;
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
  formatScaleTag(buf, len, scaleActive().label_km, s_distance_unit);
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
