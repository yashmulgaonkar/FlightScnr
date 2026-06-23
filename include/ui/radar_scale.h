#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/** One zoom band: outer ring = label_km; coverage uses 4/3 × label km for rim fetch. */
struct ScaleBand {
  float label_km;
  float coverage_km;
};

constexpr float kLabelToCoverageKm = 4.0f / 3.0f;
constexpr float kStatuteMileKm = 1.609344f;
constexpr float kNauticalMileKm = 1.852f;
constexpr float kKnotsToKmh = kNauticalMileKm;
constexpr float kKnotsToMph = 1.15077945f;
constexpr float kFeetToMeters = 0.3048f;

enum class DistanceUnit : uint8_t { Km = 0, StatuteMile = 1, NauticalMile = 2 };

constexpr ScaleBand kScaleBands[] = {
    {2.0f * kStatuteMileKm, 2.0f * kStatuteMileKm * kLabelToCoverageKm},
    {4.0f * kStatuteMileKm, 4.0f * kStatuteMileKm * kLabelToCoverageKm},
    {6.0f * kStatuteMileKm, 6.0f * kStatuteMileKm * kLabelToCoverageKm},
    {8.0f * kStatuteMileKm, 8.0f * kStatuteMileKm * kLabelToCoverageKm},
};

constexpr size_t kScaleBandCount = sizeof(kScaleBands) / sizeof(kScaleBands[0]);

void scaleBootLoad();
void scaleIncrease();
void scaleDecrease();
void scaleSelect(uint8_t index);
const ScaleBand& scaleActive();
uint8_t scaleActiveIndex();

/** ADS-B query radius (km) scaled to screen edge for rim targets. */
float adsbQueryRadiusKm();

DistanceUnit distanceUnit();
const char* distanceUnitLabel();
void cycleDistanceUnits();
void saveDistanceUnitsFromForm(const char* unit_value, const char* legacy_miles_checkbox);

bool showCompassRose();
void toggleCompassRose();
void saveCompassRoseFromForm(const char* checkbox_value);

void formatScaleTag(char* buf, size_t len, float label_km, DistanceUnit unit);
void formatActiveScaleTag(char* buf, size_t len);
void formatAltitudeDisplay(const char* alt_ft_tag, char* out, size_t out_len);
void formatSpeedLabel(char* out, size_t out_len, float gs_knots);

}  // namespace ui::radar
