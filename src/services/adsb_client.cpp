#include "services/adsb_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <esp_heap_caps.h>

#include <cctype>
#include <cmath>
#include <cstring>

#include <Preferences.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"
#include "geo/flat_earth.h"
#include "services/airport_lookup.h"
#include "services/altitude_filter.h"
#include "services/https_heap.h"
#include "services/https_lock.h"
#include "services/route_lookup.h"

namespace services::adsb {

AircraftCategory Aircraft::classify() const {
  if (isMilitary()) {
    return AircraftCategory::Military;
  }
  if (category[0] == 'A') {
    switch (category[1]) {
      case '1': return AircraftCategory::LightAircraft;
      case '2': return AircraftCategory::SmallAircraft;
      case '3': return AircraftCategory::Large;
      case '4': return AircraftCategory::Large;  // high vortex large (B757)
      case '5': return AircraftCategory::Heavy;
      case '6': return AircraftCategory::HighPerformance;
      case '7': return AircraftCategory::Helicopter;
    }
  } else if (category[0] == 'B') {
    switch (category[1]) {
      case '1': return AircraftCategory::Glider;
      case '2': return AircraftCategory::LighterThanAir;
      case '4': return AircraftCategory::LightAircraft;  // ultralight
      case '6': return AircraftCategory::UAV;
    }
  } else if (category[0] == 'C' || category[0] == 'c') {
    if (category[1] >= '1' && category[1] <= '2') {
      return AircraftCategory::GroundVehicle;
    }
  }
  // Fallback: check ICAO type for helicopter designators (Hx)
  if (type[0] == 'H' && type[1] >= '1' && type[1] <= '4') {
    return AircraftCategory::Helicopter;
  }
  return AircraftCategory::Unknown;
}

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr uint32_t kFetchHttpTimeoutMs = 8000;
constexpr uint32_t kFetchHttpTimeoutSec = kFetchHttpTimeoutMs / 1000UL;
constexpr uint32_t kFetchStallRecoveryMs = 25000;

void workerYield() { vTaskDelay(1); }

// adsb.fi v3 records carry ~45 fields; we use ~10. A deserialization filter keeps
// the JsonDocument to just those keys so a large response (many aircraft at long
// range) can't balloon internal heap and starve the SPI display driver.
void buildAircraftFilter(JsonDocument& filter) {
  JsonObject el = filter["ac"].add<JsonObject>();
  static const char* kKeepKeys[] = {
      "lat",          "lon",      "true_heading", "mag_heading", "track",
      "dir",          "gs",       "tas",          "ias",         "baro_rate",
      "geom_rate",    "alt_baro", "alt_geom",     "flight",      "hex",
      "t",            "r",        "dbFlags",      "category",    "squawk",
      "orig_icao",    "origin_icao", "dep_icao",  "from",
      "dest_icao",    "destination_icao", "arr_icao", "to"};
  for (const char* key : kKeepKeys) {
    el[key] = true;
  }
}

// The ADS-B response body and its parsed JsonDocument used to live on the
// internal heap (Arduino String + default ArduinoJson allocator). Every ~5s poll
// consumed ~25-30 KB of internal RAM in small realloc steps, fragmenting max_blk
// down to a few KB, starving lwIP, and eventually escalating into a WiFi recycle.
// Buffering in PSRAM keeps the internal heap for the WiFi/TCP stack only.
constexpr size_t kFetchMaxPayloadBytes = 262144;

struct PsramPayload {
  char* data = nullptr;
  size_t len = 0;
  size_t cap = 0;

  ~PsramPayload() {
    if (data != nullptr) {
      heap_caps_free(data);
    }
  }

  bool reserve(size_t want) {
    if (want <= cap) {
      return true;
    }
    if (want > kFetchMaxPayloadBytes) {
      return false;
    }
    void* p = heap_caps_realloc(data, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
      p = heap_caps_realloc(data, want, MALLOC_CAP_8BIT);
    }
    if (p == nullptr) {
      return false;
    }
    data = static_cast<char*>(p);
    cap = want;
    return true;
  }

  bool append(const uint8_t* src, size_t n) {
    if (len + n + 1 > cap) {
      size_t grow = cap < 8192 ? 8192 : cap * 2;
      if (grow < len + n + 1) {
        grow = len + n + 1;
      }
      if (!reserve(grow)) {
        return false;
      }
    }
    memcpy(data + len, src, n);
    len += n;
    data[len] = '\0';
    return true;
  }
};

// Read the full HTTP body with explicit connected()/available() waiting. Streaming
// ArduinoJson straight off the TLS socket is unreliable (momentary gaps look like
// EOF -> IncompleteInput on large responses), so we buffer first, then filter-parse.
bool readHttpPayload(HTTPClient& http, PsramPayload* payload, uint32_t timeout_ms) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr || payload == nullptr) {
    return false;
  }
  const int content_len = http.getSize();
  if (content_len > 0 && !payload->reserve(static_cast<size_t>(content_len) + 1)) {
    Serial.printf("[fetch] payload alloc failed (%d bytes)\n", content_len);
    return false;
  }
  const unsigned long deadline_ms = millis() + timeout_ms;
  uint8_t buf[512];
  while (http.connected() || stream->available()) {
    if (millis() >= deadline_ms) {
      if (config::kSerialTraceDebug) {
        Serial.printf("[fetch] http read timeout (%ums)\n", timeout_ms);
      }
      return false;
    }
    if (stream->available() == 0) {
      if (!http.connected()) {
        break;
      }
      workerYield();
      continue;
    }
    const size_t n = stream->readBytes(buf, sizeof(buf));
    if (n == 0) {
      workerYield();
      continue;
    }
    if (!payload->append(buf, n)) {
      Serial.println("[fetch] payload alloc failed (grow)");
      return false;
    }
  }
  return payload->len > 0;
}

void logAircraftToSerial(const Aircraft* planes, size_t count, double center_lat,
                         double center_lon) {
  (void)center_lat;
  (void)center_lon;

  static size_t s_last_logged_count = SIZE_MAX;

  if (config::kAdsbVerboseAircraftLog) {
    Serial.printf("[adsb] %u aircraft\n", static_cast<unsigned>(count));
    if (count == 0) {
      s_last_logged_count = 0;
      return;
    }

    uint8_t order[kMaxAircraft];
    float dist_km[kMaxAircraft];
    for (size_t i = 0; i < count; ++i) {
      order[i] = static_cast<uint8_t>(i);
      float dx = 0.0f;
      float dy = 0.0f;
      geo::localOffsetKm(center_lat, center_lon, planes[i].lat, planes[i].lon, &dx, &dy,
                         &dist_km[i]);
    }

    for (size_t i = 0; i + 1 < count; ++i) {
      for (size_t j = i + 1; j < count; ++j) {
        if (dist_km[order[j]] < dist_km[order[i]]) {
          const uint8_t tmp = order[i];
          order[i] = order[j];
          order[j] = tmp;
        }
      }
    }

    Serial.println(
        "  #  callsign airline              route    type alt       dist    brg  trk   gs");
    for (size_t row = 0; row < count; ++row) {
      const Aircraft& ac = planes[order[row]];
      float dx = 0.0f;
      float dy = 0.0f;
      float dist = 0.0f;
      geo::localOffsetKm(center_lat, center_lon, ac.lat, ac.lon, &dx, &dy, &dist);
      const int brg = geo::bearingFromOffset(dx, dy);
      const char* cs = ac.callsign[0] != '\0' ? ac.callsign : "-";
      const char* airline = ac.airline[0] != '\0' ? ac.airline : "-";
      const char* ty = ac.type[0] != '\0' ? ac.type : "-";
      const char* alt = ac.alt[0] != '\0' ? ac.alt : "-";
      char route[12];
      route[0] = '\0';
      if (ac.route_origin[0] != '\0' && ac.route_dest[0] != '\0') {
        snprintf(route, sizeof(route), "%s->%s", ac.route_origin, ac.route_dest);
      } else if (ac.route_origin[0] != '\0') {
        snprintf(route, sizeof(route), "%s->?", ac.route_origin);
      } else if (ac.route_dest[0] != '\0') {
        snprintf(route, sizeof(route), "?->%s", ac.route_dest);
      } else {
        strncpy(route, "-", sizeof(route) - 1);
        route[sizeof(route) - 1] = '\0';
      }
      Serial.printf(" %2u  %-7s %-22s %-8s %-4s %-9s %5.1f km %03d deg %4.0f deg %4.0f kt\n",
                    static_cast<unsigned>(row + 1), cs, airline, route, ty, alt, dist, brg,
                    ac.track_deg, ac.gs_knots);
    }
    s_last_logged_count = count;
    return;
  }

  if (count == s_last_logged_count) {
    return;
  }
  s_last_logged_count = count;
  Serial.printf("[adsb] %u aircraft\n", static_cast<unsigned>(count));
}

constexpr char kPrefsNamespace[] = "flightscnr";
constexpr char kPrefsAltFloorKey[] = "alt_floor_ft";
constexpr char kPrefsAltFloorEnKey[] = "alt_fl_en";
constexpr char kLegacyAltFloorKey[] = "minAltFt";
constexpr char kPrefsAltCeilKey[] = "alt_ceil_ft";

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
int s_altitude_floor_ft = config::kFactoryAltitudeFloorFt;
int s_altitude_ceiling_ft = config::kFactoryAltitudeCeilingFt;

Aircraft s_aircraft_staging[kMaxAircraft];
size_t s_aircraft_staging_count = 0;

SemaphoreHandle_t s_aircraft_mutex = nullptr;
TaskHandle_t s_fetch_task = nullptr;

volatile bool s_fetch_requested = false;
volatile bool s_fetch_ready = false;
volatile bool s_fetch_busy = false;

// One-shot job borrowed onto this worker (see queueBackgroundJob).
volatile BackgroundJob s_bg_job = nullptr;
volatile bool s_bg_job_busy = false;

unsigned long s_last_fetch_ok_ms = 0;
unsigned long s_fetch_busy_since_ms = 0;
uint32_t s_fetch_fail_streak = 0;

// adsb.fi returned HTTP 429 (rate limited). Tracked separately from the TLS
// fail streak: rate limiting must not trigger a WiFi recycle, just a back-off.
volatile bool s_fetch_rate_limited = false;
volatile unsigned long s_rate_limit_until_ms = 0;

double s_fetch_lat = 0.0;
double s_fetch_lon = 0.0;
float s_fetch_radius_km = 0.0f;

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  JsonVariantConst v = obj[key];
  // Cover the integer/float forms ArduinoJson 7 may store. Do not call as<float>()
  // on strings — "ground" must not become 0 via conversion.
  if (!(v.is<float>() || v.is<double>() || v.is<int>() || v.is<long>() ||
        v.is<unsigned int>() || v.is<unsigned long>())) {
    return false;
  }
  *out = v.as<float>();
  return true;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool pickVerticalRateFpm(const JsonObject& plane, int16_t* out_fpm) {
  float v = 0.0f;
  if (!readJsonFloat(plane, "baro_rate", &v) && !readJsonFloat(plane, "geom_rate", &v)) {
    return false;
  }
  const long rounded = lroundf(v);
  if (rounded < INT16_MIN) {
    *out_fpm = INT16_MIN;
  } else if (rounded > INT16_MAX) {
    *out_fpm = INT16_MAX;
  } else {
    *out_fpm = static_cast<int16_t>(rounded);
  }
  return true;
}

bool isOnGround(const JsonObject& plane) {
  // Prefer as<> over is<>: filtered docs sometimes reject is<const char*>()
  // even when the string value is present.
  const char* s = plane["alt_baro"].as<const char*>();
  if (s == nullptr || s[0] == '\0') {
    return false;
  }
  // Case-insensitive "ground" (feeds use lowercase; be tolerant).
  if ((s[0] == 'g' || s[0] == 'G') && (s[1] == 'r' || s[1] == 'R') &&
      (s[2] == 'o' || s[2] == 'O') && (s[3] == 'u' || s[3] == 'U') &&
      (s[4] == 'n' || s[4] == 'N') && (s[5] == 'd' || s[5] == 'D') &&
      s[6] == '\0') {
    return true;
  }
  return false;
}

/** ADS-B emitter categories C1/C2 are surface vehicles (trucks, etc.). */
bool isGroundVehicleCategory(const JsonObject& plane) {
  const char* cat = plane["category"].as<const char*>();
  if (cat == nullptr || cat[0] == '\0') {
    return false;
  }
  const char c0 = static_cast<char>(toupper(static_cast<unsigned char>(cat[0])));
  return c0 == 'C' && (cat[1] == '1' || cat[1] == '2') &&
         (cat[2] == '\0' || cat[2] == ' ');
}

bool isGroundTraffic(const JsonObject& plane) {
  return isOnGround(plane) || isGroundVehicleCategory(plane);
}

bool readAltitudeFt(const JsonObject& plane, float* out_ft) {
  if (isOnGround(plane)) {
    // Treat ADS-B "ground" as 0 ft so min-floor 0 includes taxi/ground traffic
    // and a positive floor still filters them out.
    *out_ft = 0.0f;
    return true;
  }
  if (readJsonFloat(plane, "alt_baro", out_ft)) {
    return true;
  }
  return readJsonFloat(plane, "alt_geom", out_ft);
}

bool passesAltitudeBand(const JsonObject& plane) {
  // Ground / surface vehicles: included only when the min floor is off (0).
  // Do this before the generic band check so a missing numeric altitude cannot
  // drop trucks when the user explicitly disabled the floor.
  if (isGroundTraffic(plane)) {
    return s_altitude_floor_ft <= 0;
  }
  if (s_altitude_floor_ft <= 0 && s_altitude_ceiling_ft <= 0) {
    return true;
  }
  float alt_ft = 0.0f;
  const bool has = readAltitudeFt(plane, &alt_ft);
  return services::adsb::altitudeWithinBand(has, alt_ft, s_altitude_floor_ft,
                                            s_altitude_ceiling_ft);
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }
  // Prefer as<> — is<const char*>() can fail on filtered JsonDocuments.
  const char* s = obj[key].as<const char*>();
  if (s == nullptr || s[0] == '\0') {
    return;
  }
  while (*s == ' ') {
    ++s;
  }
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (isGroundTraffic(plane)) {
    strncpy(out, "GND", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void copyIcaoCode(const char* s, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (s == nullptr || s[0] == '\0') {
    return;
  }
  size_t n = 0;
  while (s[n] != '\0' && n + 1 < out_len && n < 4) {
    const unsigned char c = static_cast<unsigned char>(s[n]);
    out[n] = static_cast<char>(isupper(c) ? c : toupper(c));
    ++n;
  }
  out[n] = '\0';
}

void fillRouteIcaoFromAdsb(Aircraft* ac, const JsonObject& plane) {
  if (ac == nullptr) {
    return;
  }
  ac->route_origin[0] = '\0';
  ac->route_dest[0] = '\0';

  static const char* kOriginKeys[] = {"orig_icao", "origin_icao", "dep_icao", "from"};
  static const char* kDestKeys[] = {"dest_icao", "destination_icao", "arr_icao", "to"};

  for (const char* key : kOriginKeys) {
    if (!plane[key].is<const char*>()) {
      continue;
    }
    copyIcaoCode(plane[key].as<const char*>(), ac->route_origin, sizeof(ac->route_origin));
    char resolved[5];
    if (services::airport::normalizeRouteCode(ac->route_origin, resolved, sizeof(resolved))) {
      copyIcaoCode(resolved, ac->route_origin, sizeof(ac->route_origin));
    }
    if (ac->route_origin[0] != '\0' && ac->route_origin[3] != '\0') {
      break;
    }
    ac->route_origin[0] = '\0';
  }

  for (const char* key : kDestKeys) {
    if (!plane[key].is<const char*>()) {
      continue;
    }
    copyIcaoCode(plane[key].as<const char*>(), ac->route_dest, sizeof(ac->route_dest));
    char resolved[5];
    if (services::airport::normalizeRouteCode(ac->route_dest, resolved, sizeof(resolved))) {
      copyIcaoCode(resolved, ac->route_dest, sizeof(ac->route_dest));
    }
    if (ac->route_dest[0] != '\0' && ac->route_dest[3] != '\0') {
      break;
    }
    ac->route_dest[0] = '\0';
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  char flight_id[sizeof(ac->callsign)];
  flight_id[0] = '\0';
  copyJsonStringTrimmed(plane, "flight", flight_id, sizeof(flight_id));
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  for (size_t i = 0; ac->hex[i] != '\0'; ++i) {
    ac->hex[i] = static_cast<char>(toupper(static_cast<unsigned char>(ac->hex[i])));
  }

  const bool has_flight = flight_id[0] != '\0';
  if (has_flight) {
    strncpy(ac->callsign, flight_id, sizeof(ac->callsign) - 1);
    ac->callsign[sizeof(ac->callsign) - 1] = '\0';
  } else if (ac->hex[0] != '\0') {
    strncpy(ac->callsign, ac->hex, sizeof(ac->callsign) - 1);
    ac->callsign[sizeof(ac->callsign) - 1] = '\0';
  } else {
    ac->callsign[0] = '\0';
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  copyJsonStringTrimmed(plane, "r", ac->registration, sizeof(ac->registration));
  for (size_t i = 0; ac->registration[i] != '\0'; ++i) {
    ac->registration[i] =
        static_cast<char>(toupper(static_cast<unsigned char>(ac->registration[i])));
  }
  copyJsonStringTrimmed(plane, "category", ac->category, sizeof(ac->category));
  copyJsonStringTrimmed(plane, "squawk", ac->squawk, sizeof(ac->squawk));
  ac->db_flags = static_cast<uint8_t>(plane["dbFlags"] | 0);
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
  if (!pickVerticalRateFpm(plane, &ac->vert_rate_fpm)) {
    ac->vert_rate_fpm = kVertRateUnknown;
  }
  ac->airline[0] = '\0';
  ac->airline_icao[0] = '\0';
  fillRouteIcaoFromAdsb(ac, plane);
}

void applyRouteFieldsByCallsignImpl(const char* callsign, const char* airline,
                                    const char* airline_icao, const char* origin,
                                    const char* dest) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return;
  }

  if (s_aircraft_mutex != nullptr) {
    xSemaphoreTake(s_aircraft_mutex, portMAX_DELAY);
  }

  for (size_t i = 0; i < s_aircraft_count; ++i) {
    if (strcmp(s_aircraft[i].callsign, callsign) != 0) {
      continue;
    }
    Aircraft& ac = s_aircraft[i];
    if (airline != nullptr && airline[0] != '\0') {
      strncpy(ac.airline, airline, sizeof(ac.airline) - 1);
      ac.airline[sizeof(ac.airline) - 1] = '\0';
    }
    if (airline_icao != nullptr && airline_icao[0] != '\0') {
      strncpy(ac.airline_icao, airline_icao, sizeof(ac.airline_icao) - 1);
      ac.airline_icao[sizeof(ac.airline_icao) - 1] = '\0';
    }
    if (origin != nullptr && origin[0] != '\0') {
      char resolved[5];
      if (services::airport::normalizeRouteCode(origin, resolved, sizeof(resolved))) {
        strncpy(ac.route_origin, resolved, sizeof(ac.route_origin) - 1);
      } else {
        strncpy(ac.route_origin, origin, sizeof(ac.route_origin) - 1);
      }
      ac.route_origin[sizeof(ac.route_origin) - 1] = '\0';
    }
    if (dest != nullptr && dest[0] != '\0') {
      char resolved[5];
      if (services::airport::normalizeRouteCode(dest, resolved, sizeof(resolved))) {
        strncpy(ac.route_dest, resolved, sizeof(ac.route_dest) - 1);
      } else {
        strncpy(ac.route_dest, dest, sizeof(ac.route_dest) - 1);
      }
      ac.route_dest[sizeof(ac.route_dest) - 1] = '\0';
    }
    break;
  }

  if (s_aircraft_mutex != nullptr) {
    xSemaphoreGive(s_aircraft_mutex);
  }
}

}  // namespace

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

size_t copyAircraftSnapshot(Aircraft* dst, size_t max_count) {
  if (dst == nullptr || max_count == 0) {
    return 0;
  }
  if (s_aircraft_mutex != nullptr) {
    xSemaphoreTake(s_aircraft_mutex, portMAX_DELAY);
  }
  const size_t n = s_aircraft_count <= max_count ? s_aircraft_count : max_count;
  if (n > 0) {
    memcpy(dst, s_aircraft, n * sizeof(Aircraft));
  }
  if (s_aircraft_mutex != nullptr) {
    xSemaphoreGive(s_aircraft_mutex);
  }
  return n;
}

bool copyAircraftAt(size_t index, Aircraft* dst) {
  if (dst == nullptr) {
    return false;
  }
  if (s_aircraft_mutex != nullptr) {
    xSemaphoreTake(s_aircraft_mutex, portMAX_DELAY);
  }
  const bool ok = index < s_aircraft_count;
  if (ok) {
    *dst = s_aircraft[index];
  }
  if (s_aircraft_mutex != nullptr) {
    xSemaphoreGive(s_aircraft_mutex);
  }
  return ok;
}

bool copyAircraftByCallsign(const char* callsign, Aircraft* dst) {
  if (dst == nullptr || callsign == nullptr || callsign[0] == '\0') {
    return false;
  }
  if (s_aircraft_mutex != nullptr) {
    xSemaphoreTake(s_aircraft_mutex, portMAX_DELAY);
  }
  bool ok = false;
  for (size_t i = 0; i < s_aircraft_count; ++i) {
    if (strcmp(s_aircraft[i].callsign, callsign) == 0) {
      *dst = s_aircraft[i];
      ok = true;
      break;
    }
  }
  if (s_aircraft_mutex != nullptr) {
    xSemaphoreGive(s_aircraft_mutex);
  }
  return ok;
}

void applyRouteFieldsByCallsign(const char* callsign, const char* airline,
                                const char* airline_icao, const char* origin,
                                const char* dest) {
  applyRouteFieldsByCallsignImpl(callsign, airline, airline_icao, origin, dest);
}

void trafficFilterBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    s_altitude_floor_ft = config::kFactoryAltitudeFloorFt;
    s_altitude_ceiling_ft = config::kFactoryAltitudeCeilingFt;
    return;
  }
  // Use an explicit enable flag so floor=0 cannot be confused with a missing
  // NVS key (which previously fell back to the legacy/default 500 ft).
  if (prefs.isKey(kPrefsAltFloorEnKey)) {
    if (!prefs.getBool(kPrefsAltFloorEnKey, true)) {
      s_altitude_floor_ft = 0;
    } else {
      s_altitude_floor_ft =
          prefs.getInt(kPrefsAltFloorKey, config::kFactoryAltitudeFloorFt);
      if (s_altitude_floor_ft <= 0) {
        s_altitude_floor_ft = config::kFactoryAltitudeFloorFt;
      }
    }
  } else if (prefs.isKey(kPrefsAltFloorKey)) {
    s_altitude_floor_ft =
        prefs.getInt(kPrefsAltFloorKey, config::kFactoryAltitudeFloorFt);
  } else {
    s_altitude_floor_ft =
        prefs.getInt(kLegacyAltFloorKey, config::kFactoryAltitudeFloorFt);
  }
  if (s_altitude_floor_ft < 0) {
    s_altitude_floor_ft = 0;
  }
  s_altitude_ceiling_ft =
      prefs.getInt(kPrefsAltCeilKey, config::kFactoryAltitudeCeilingFt);
  if (s_altitude_ceiling_ft < 0) {
    s_altitude_ceiling_ft = 0;
  }
  prefs.end();

  Serial.printf("Traffic altitude floor: %s (%d ft)\n",
                s_altitude_floor_ft > 0 ? "on" : "off/include-ground",
                s_altitude_floor_ft);
  if (s_altitude_ceiling_ft > 0) {
    Serial.printf("Traffic altitude ceiling: %d ft\n", s_altitude_ceiling_ft);
  }
}

int altitudeFloorFt() { return s_altitude_floor_ft; }

int altitudeCeilingFt() { return s_altitude_ceiling_ft; }

void saveAltitudeFloorFromForm(const char* value) {
  int min_ft = 0;
  if (value != nullptr && value[0] != '\0') {
    min_ft = static_cast<int>(lroundf(strtof(value, nullptr)));
  }
  if (min_ft < 0) {
    min_ft = 0;
  }
  Preferences prefs;
  bool stored = false;
  if (prefs.begin(kPrefsNamespace, false)) {
    // Explicit enable flag: floor 0 must survive reboot (NVS int alone is easy
    // to mis-read as "unset" → legacy default 500).
    const bool en = min_ft > 0;
    stored = prefs.putBool(kPrefsAltFloorEnKey, en);
    if (en) {
      stored = prefs.putInt(kPrefsAltFloorKey, min_ft) > 0 && stored;
    } else {
      prefs.putInt(kPrefsAltFloorKey, 0);
    }
    if (prefs.isKey(kLegacyAltFloorKey)) {
      prefs.remove(kLegacyAltFloorKey);
    }
    prefs.end();
  }
  s_altitude_floor_ft = min_ft;
  Serial.printf("Traffic altitude floor: %s (%d ft) nvs=%d raw=\"%s\"\n",
                min_ft > 0 ? "on" : "off/include-ground", min_ft, stored ? 1 : 0,
                value != nullptr ? value : "");
}

void saveAltitudeCeilingFromForm(const char* value) {
  int max_ft = 0;
  if (value != nullptr && value[0] != '\0') {
    max_ft = static_cast<int>(lroundf(strtof(value, nullptr)));
  }
  if (max_ft < 0) {
    max_ft = 0;
  }
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putInt(kPrefsAltCeilKey, max_ft);
    prefs.end();
  }
  s_altitude_ceiling_ft = max_ft;
  if (max_ft > 0) {
    Serial.printf("Traffic altitude ceiling: %d ft\n", max_ft);
  } else {
    Serial.println("Traffic altitude ceiling off");
  }
}

float distSqFromCenterKm(double center_lat, double center_lon, float lat,
                         float lon) {
  float east_km = 0.0f;
  float north_km = 0.0f;
  geo::localOffsetKm(center_lat, center_lon, lat, lon, &east_km, &north_km,
                     nullptr);
  return east_km * east_km + north_km * north_km;
}

bool parseAircraftDoc(JsonDocument& doc, Aircraft* out, size_t* out_count,
                      double center_lat, double center_lon) {
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    *out_count = 0;
    return true;
  }

  // Keep the nearest kMaxAircraft. The feed is not distance-sorted; with the
  // altitude floor off, a large radius can exceed the cap and a naive first-N
  // take drops nearby taxi/ground traffic (including trucks).
  float dist_sq[kMaxAircraft];
  size_t n = 0;
  size_t considered = 0;
  size_t ground_kept = 0;
  size_t farthest_i = 0;
  float farthest_d2 = 0.0f;

  for (JsonObject plane : ac) {
    float lat = 0.0f;
    float lon = 0.0f;
    if (!readJsonFloat(plane, "lat", &lat) || !readJsonFloat(plane, "lon", &lon)) {
      continue;
    }
    if (!passesAltitudeBand(plane)) {
      continue;
    }
    ++considered;

    const float d2 = distSqFromCenterKm(center_lat, center_lon, lat, lon);
    size_t slot = n;
    if (n < kMaxAircraft) {
      dist_sq[n] = d2;
      if (n == 0 || d2 > farthest_d2) {
        farthest_d2 = d2;
        farthest_i = n;
      }
      ++n;
    } else if (d2 < farthest_d2) {
      slot = farthest_i;
      dist_sq[slot] = d2;
      farthest_d2 = dist_sq[0];
      farthest_i = 0;
      for (size_t i = 1; i < kMaxAircraft; ++i) {
        if (dist_sq[i] > farthest_d2) {
          farthest_d2 = dist_sq[i];
          farthest_i = i;
        }
      }
    } else {
      continue;
    }

    out[slot].lat = lat;
    out[slot].lon = lon;
    out[slot].nose_deg = pickNoseHeading(plane);
    out[slot].track_deg = pickTrackHeading(plane);
    out[slot].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&out[slot], plane);
  }

  for (size_t i = 0; i < n; ++i) {
    if (strcmp(out[i].alt, "GND") == 0 ||
        (out[i].category[0] == 'C' &&
         (out[i].category[1] == '1' || out[i].category[1] == '2') &&
         out[i].category[2] == '\0')) {
      ++ground_kept;
    }
  }

  *out_count = n;
  // Always log so serial can confirm floor + ground ingestion after a flash.
  Serial.printf("[adsb] floor=%d ceil=%d kept=%u/%u ground=%u\n",
                s_altitude_floor_ft, s_altitude_ceiling_ft,
                static_cast<unsigned>(n), static_cast<unsigned>(considered),
                static_cast<unsigned>(ground_kept));
  return true;
}

// PSRAM-first allocator so the filtered JsonDocument never fragments internal
// heap (mirrors RouteJsonAllocator in route_lookup.cpp).
struct AdsbJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == nullptr) {
      ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
  }
  void deallocate(void* ptr) override {
    if (ptr != nullptr) {
      heap_caps_free(ptr);
    }
  }
  void* reallocate(void* ptr, size_t new_size) override {
    void* out = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == nullptr) {
      out = heap_caps_realloc(ptr, new_size, MALLOC_CAP_8BIT);
    }
    return out;
  }
};

AdsbJsonAllocator s_adsb_json_allocator;

// Parse a buffered response body with a field filter, so only the ~10 keys we use
// are materialized in the JsonDocument (keeps peak heap low on big responses).
bool parseAircraftPayload(const char* payload, size_t payload_len, Aircraft* out,
                          size_t* out_count, double center_lat,
                          double center_lon) {
  JsonDocument filter(&s_adsb_json_allocator);
  buildAircraftFilter(filter);

  JsonDocument doc(&s_adsb_json_allocator);
  const DeserializationError err =
      deserializeJson(doc, payload, payload_len, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[adsb] JSON parse error: %s\n", err.c_str());
    return false;
  }
  return parseAircraftDoc(doc, out, out_count, center_lat, center_lon);
}

bool fetchUpdateBlocking(double center_lat, double center_lon, float fetch_radius_km,
                       Aircraft* out, size_t* out_count) {
  const unsigned long fetch_start_ms = millis();
  if (config::kSerialTraceDebug) {
    Serial.println("[fetch] begin HTTPS");
  }
  if (!services::https::heapReadyForAdsb()) {
    if (config::kSerialTraceDebug) {
      Serial.printf("[fetch] skip heap free=%u max_blk=%u\n", ESP.getFreeHeap(),
                    ESP.getMaxAllocHeap());
    }
    return false;
  }
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  services::https::ScopedLock tls(kFetchHttpTimeoutMs + 2000);
  if (!tls.held()) {
    if (config::kSerialTraceDebug) {
      Serial.println("[fetch] skip: https lock busy");
    }
    Serial.println("[adsb] HTTPS busy (fetch skipped)");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  // WiFiClientSecure::setTimeout/setHandshakeTimeout take seconds, not milliseconds.
  client.setTimeout(kFetchHttpTimeoutSec);
  client.setHandshakeTimeout(kFetchHttpTimeoutSec);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[adsb] http.begin failed");
    return false;
  }

  http.setConnectTimeout(kFetchHttpTimeoutMs);
  http.setTimeout(kFetchHttpTimeoutMs);
  http.setReuse(false);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code == 429) {
      // Rate limited: back off, but do NOT treat as a TLS failure (no WiFi recycle).
      s_fetch_rate_limited = true;
      s_rate_limit_until_ms = millis() + config::kAdsbRateLimitBackoffMs;
      Serial.printf("[adsb] HTTP 429 (rate limited) — backing off %lums\n",
                    config::kAdsbRateLimitBackoffMs);
    } else {
      s_fetch_rate_limited = false;
      // Only a failed connect (TLS handshake could not allocate/complete) or an
      // explicit out-of-RAM error means memory pressure worth a WiFi recycle.
      // Read timeouts (-11) and dropped connections (-5) are usually transient
      // network conditions; they feed the fail streak, and the streak-based
      // recycle in tryTlsWifiRefresh() remains the last resort.
      if (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_TOO_LESS_RAM) {
        services::route::noteTlsMemoryFailure();
      }
      if (code == HTTPC_ERROR_CONNECTION_REFUSED) {
        Serial.println("[adsb] HTTP -1 (TLS connect failed, see start_ssl_client above)");
      } else {
        Serial.printf("[adsb] HTTP %d\n", code);
      }
    }
    http.end();
    client.stop();
    services::https::drainTlsHeapAfterSession();
    return false;
  }

  PsramPayload payload;
  if (!readHttpPayload(http, &payload, kFetchHttpTimeoutMs + 4000U)) {
    http.end();
    client.stop();
    return false;
  }
  http.end();
  client.stop();

  services::https::drainTlsHeapAfterSession();
  workerYield();
  if (!parseAircraftPayload(payload.data, payload.len, out, out_count, center_lat,
                            center_lon)) {
    if (config::kSerialTraceDebug) {
      Serial.printf("[fetch] fail parse (%lums)\n", millis() - fetch_start_ms);
    }
    return false;
  }

  if (config::kSerialTraceDebug) {
    Serial.printf("[fetch] ok %u aircraft (%lums)\n", static_cast<unsigned>(*out_count),
                  millis() - fetch_start_ms);
  }
  return true;
}

void fetchWorkerTask(void* /*arg*/) {
  for (;;) {
    // A queued one-shot job (weather) takes priority over the next ADS-B poll:
    // it is user-initiated and rare, and on the radar screen ADS-B re-requests
    // every cycle, so checking it first is the only way weather ever gets a turn.
    if (s_bg_job != nullptr) {
      s_bg_job_busy = true;
      BackgroundJob job = s_bg_job;
      job();
      s_bg_job = nullptr;
      s_bg_job_busy = false;
    } else if (s_fetch_requested) {
      s_fetch_busy = true;
      s_fetch_busy_since_ms = millis();
      size_t n = 0;
      const bool ok = fetchUpdateBlocking(s_fetch_lat, s_fetch_lon, s_fetch_radius_km,
                                        s_aircraft_staging, &n);
      if (ok) {
        s_last_fetch_ok_ms = millis();
        s_fetch_fail_streak = 0;
        s_fetch_rate_limited = false;
        s_rate_limit_until_ms = 0;
        s_aircraft_staging_count = n;
        s_fetch_ready = true;
      } else if (!s_fetch_rate_limited) {
        // Genuine TLS/connection/parse failure feeds the WiFi-recycle streak;
        // a 429 (rate limit) is handled by the back-off timer instead.
        ++s_fetch_fail_streak;
      }
      s_fetch_requested = false;
      s_fetch_busy = false;
      s_fetch_busy_since_ms = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void fetchInit() {
  if (s_fetch_task != nullptr) {
    return;
  }
  services::https::init();
  if (s_aircraft_mutex == nullptr) {
    s_aircraft_mutex = xSemaphoreCreateMutex();
  }
  // Pin to core 0 (PRO_CPU, alongside the WiFi/TLS stack) so heavy HTTPS/mbedTLS
  // CPU work never starves the render loop, which runs on core 1 (ARDUINO_RUNNING_CORE).
  xTaskCreatePinnedToCore(fetchWorkerTask, "adsb_fetch", 16384, nullptr, 1, &s_fetch_task,
                          config::kCoreNetwork);
}

bool queueBackgroundJob(BackgroundJob job) {
  if (job == nullptr) {
    return false;
  }
  if (s_fetch_task == nullptr) {
    fetchInit();
  }
  if (s_bg_job != nullptr || s_bg_job_busy) {
    return false;
  }
  s_bg_job = job;
  return true;
}

bool backgroundJobActive() { return s_bg_job != nullptr || s_bg_job_busy; }

void cancelPendingFetch() {
  if (!s_fetch_busy) {
    s_fetch_requested = false;
  }
}

bool fetchRequest(double center_lat, double center_lon, float fetch_radius_km) {
  if (s_fetch_task == nullptr) {
    fetchInit();
  }
  if (s_fetch_requested || s_fetch_busy) {
    return false;
  }
  s_fetch_lat = center_lat;
  s_fetch_lon = center_lon;
  s_fetch_radius_km = fetch_radius_km;
  s_fetch_requested = true;
  return true;
}

bool fetchReady() { return s_fetch_ready; }

void fetchProcessReady(const bool enrich_routes) {
  if (!s_fetch_ready) {
    return;
  }

  if (enrich_routes) {
    services::route::enrichAircraft(s_aircraft_staging, s_aircraft_staging_count, s_fetch_lat,
                                    s_fetch_lon);
  }
  logAircraftToSerial(s_aircraft_staging, s_aircraft_staging_count, s_fetch_lat, s_fetch_lon);

  if (s_aircraft_mutex != nullptr) {
    xSemaphoreTake(s_aircraft_mutex, portMAX_DELAY);
  }
  memcpy(s_aircraft, s_aircraft_staging, s_aircraft_staging_count * sizeof(Aircraft));
  s_aircraft_count = s_aircraft_staging_count;
  if (s_aircraft_mutex != nullptr) {
    xSemaphoreGive(s_aircraft_mutex);
  }
  s_fetch_ready = false;
}

void fetchConsume() { s_fetch_ready = false; }

void fetchWatchdog(unsigned long now_ms) {
  if (!s_fetch_requested && !s_fetch_busy) {
    return;
  }
  if (s_fetch_busy_since_ms == 0) {
    return;
  }
  if (now_ms - s_fetch_busy_since_ms < kFetchStallRecoveryMs) {
    return;
  }

  Serial.printf("[adsb] fetch stall recovery (%lums busy)\n",
                now_ms - s_fetch_busy_since_ms);
  if (s_fetch_task != nullptr) {
    // Worker may hold the HTTPS lock / stack C++ clients; reclaim if this task
    // owned it (or left an orphan holder) — never steal from the route worker.
    TaskHandle_t task = s_fetch_task;
    s_fetch_task = nullptr;
    vTaskDelete(task);
    services::https::reclaimAfterTaskDeleted(task);
  }
  s_fetch_requested = false;
  s_fetch_busy = false;
  s_fetch_ready = false;
  s_fetch_busy_since_ms = 0;
  ++s_fetch_fail_streak;
  fetchInit();
}

bool fetchInProgress() { return s_fetch_requested || s_fetch_busy; }

uint32_t lastFetchOkAgeMs() {
  if (s_last_fetch_ok_ms == 0) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(millis() - s_last_fetch_ok_ms);
}

uint32_t fetchFailStreak() { return s_fetch_fail_streak; }

bool rateLimitBackoffActive(unsigned long now_ms) {
  return s_rate_limit_until_ms != 0 && now_ms < s_rate_limit_until_ms;
}

void fetchResetFailStreak() { s_fetch_fail_streak = 0; }

uint32_t fetchTaskStackFreeBytes() {
  if (s_fetch_task == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_fetch_task) * sizeof(StackType_t));
}

}  // namespace services::adsb
