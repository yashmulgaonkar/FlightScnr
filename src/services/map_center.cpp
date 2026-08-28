#include "services/map_center.h"

#include <Preferences.h>
#include <cstdlib>

#include "config.h"

namespace services::map_center {

namespace {

constexpr char kStoreNs[] = "fs_geo";
constexpr char kLatKey[] = "center_lat";
constexpr char kLonKey[] = "center_lon";

/** Legacy NVS layout (pre–clean-room refactor). */
constexpr char kLegacyNs[] = "radar";
constexpr char kLegacyLatKey[] = "lat";
constexpr char kLegacyLonKey[] = "lon";

double s_latitude = config::kFactoryLatitude;
double s_longitude = config::kFactoryLongitude;

bool parseDegrees(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validCoordinates(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

void writeFlash(double lat, double lon) {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  prefs.putDouble(kLatKey, lat);
  prefs.putDouble(kLonKey, lon);
  prefs.end();
  s_latitude = lat;
  s_longitude = lon;
}

bool readFromNamespace(const char* ns, const char* lat_key, const char* lon_key,
                       double* lat, double* lon) {
  Preferences prefs;
  if (!prefs.begin(ns, true)) {
    return false;
  }
  if (!prefs.isKey(lat_key) || !prefs.isKey(lon_key)) {
    prefs.end();
    return false;
  }
  const double la = prefs.getDouble(lat_key, config::kFactoryLatitude);
  const double lo = prefs.getDouble(lon_key, config::kFactoryLongitude);
  prefs.end();
  if (!validCoordinates(la, lo)) {
    return false;
  }
  *lat = la;
  *lon = lo;
  return true;
}

}  // namespace

void bootLoad() {
  double lat = config::kFactoryLatitude;
  double lon = config::kFactoryLongitude;
  if (readFromNamespace(kStoreNs, kLatKey, kLonKey, &lat, &lon)) {
    s_latitude = lat;
    s_longitude = lon;
    Serial.printf("Map center: %.6f, %.6f (saved)\n", lat, lon);
    return;
  }
  if (readFromNamespace(kLegacyNs, kLegacyLatKey, kLegacyLonKey, &lat, &lon)) {
    s_latitude = lat;
    s_longitude = lon;
    writeFlash(lat, lon);
    Serial.printf("Map center: %.6f, %.6f (legacy migrated)\n", lat, lon);
    return;
  }
  s_latitude = config::kFactoryLatitude;
  s_longitude = config::kFactoryLongitude;
  Serial.printf("Map center: %.6f, %.6f (factory default)\n", s_latitude, s_longitude);
}

double latitude() { return s_latitude; }

double longitude() { return s_longitude; }

bool applyPortalCoordinates(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!parseDegrees(lat_str, &lat) || !parseDegrees(lon_str, &lon)) {
    return false;
  }
  if (!validCoordinates(lat, lon)) {
    return false;
  }
  writeFlash(lat, lon);
  Serial.printf("Map center saved: %.6f, %.6f\n", lat, lon);
  return true;
}

bool applyRadarCenterFromForm(const char* center_str) {
  if (center_str == nullptr || center_str[0] == '\0') {
    return false;
  }
  const char* p = center_str;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }

  char* end = nullptr;
  const double lat = strtod(p, &end);
  if (end == p) {
    return false;
  }
  p = end;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (*p != ',') {
    return false;
  }
  ++p;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }

  const double lon = strtod(p, &end);
  if (end == p) {
    return false;
  }
  while (*end == ' ' || *end == '\t') {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }
  if (!validCoordinates(lat, lon)) {
    return false;
  }
  writeFlash(lat, lon);
  Serial.printf("Map center saved: %.6f, %.6f\n", lat, lon);
  return true;
}

}  // namespace services::map_center
