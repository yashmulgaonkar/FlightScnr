#include "ui/radar_accent.h"

#include <Preferences.h>

#include <cstdlib>
#include <cstring>

#include "services/settings_state.h"

namespace ui::radar {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kAccentKey[] = "radar_accent";

constexpr RadarAccentColor kDefaultAccent = RadarAccentColor::Green;

RadarAccentColor s_accent = kDefaultAccent;

constexpr AccentRgb kPalettes[] = {
    {200, 24, 24, 255, 64, 64, 72, 8, 8, 255, 80, 80},
    {180, 180, 0, 255, 255, 64, 72, 72, 0, 255, 255, 128},
    {200, 100, 0, 255, 180, 48, 80, 40, 0, 255, 200, 64},
    {16, 100, 32, 48, 255, 96, 12, 72, 28, 128, 255, 160},
    {160, 160, 160, 255, 255, 255, 80, 80, 80, 255, 255, 255},
};

constexpr const char* kNames[] = {"Red", "Yellow", "Orange", "Green", "White"};

void persistAccent() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kAccentKey, static_cast<uint8_t>(s_accent));
    prefs.end();
  }
  settingsStateBump();
}

}  // namespace

void accentBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  const uint8_t stored = prefs.getUChar(kAccentKey, static_cast<uint8_t>(kDefaultAccent));
  prefs.end();
  if (stored < kRadarAccentCount) {
    s_accent = static_cast<RadarAccentColor>(stored);
  }
}

RadarAccentColor accentColor() { return s_accent; }

const char* accentColorName() {
  const uint8_t idx = static_cast<uint8_t>(s_accent);
  if (idx >= kRadarAccentCount) {
    return kNames[static_cast<uint8_t>(kDefaultAccent)];
  }
  return kNames[idx];
}

uint8_t accentColorIndex() { return static_cast<uint8_t>(s_accent); }

const char* accentColorNameAt(uint8_t index) {
  if (index >= kRadarAccentCount) {
    return "";
  }
  return kNames[index];
}

bool accentSaveFromForm(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const long idx = strtol(value, &end, 10);
  if (end == value || idx < 0 || idx >= static_cast<long>(kRadarAccentCount)) {
    return false;
  }
  const RadarAccentColor next = static_cast<RadarAccentColor>(idx);
  if (next != s_accent) {
    s_accent = next;
    persistAccent();
    Serial.printf("Radar accent (web): %s\n", accentColorName());
  }
  return true;
}

AccentRgb accentPalette() {
  const uint8_t idx = static_cast<uint8_t>(s_accent);
  if (idx >= kRadarAccentCount) {
    return kPalettes[static_cast<uint8_t>(kDefaultAccent)];
  }
  return kPalettes[idx];
}

void accentHighlightRgb(uint8_t* r, uint8_t* g, uint8_t* b) {
  const AccentRgb palette = accentPalette();
  if (r != nullptr) {
    *r = palette.sweep_r;
  }
  if (g != nullptr) {
    *g = palette.sweep_g;
  }
  if (b != nullptr) {
    *b = palette.sweep_b;
  }
}

void accentStep(int8_t delta) {
  if (delta == 0) {
    return;
  }
  int idx = static_cast<int>(s_accent);
  idx = (idx + delta) % static_cast<int>(kRadarAccentCount);
  if (idx < 0) {
    idx += static_cast<int>(kRadarAccentCount);
  }
  s_accent = static_cast<RadarAccentColor>(idx);
  persistAccent();
  Serial.printf("Radar accent: %s\n", accentColorName());
}

}  // namespace ui::radar
