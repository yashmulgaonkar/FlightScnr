#include "services/route_lookup.h"

#include <Arduino.h>

#include "services/adsb_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cctype>
#include <cstring>
#include <ctime>

#include "config.h"
#include "geo/flat_earth.h"
#include "services/airline_lookup.h"
#include "services/airport_lookup.h"
#include "services/api_keys.h"
#include "services/https_lock.h"
#include "services/route_cache_store.h"

namespace services::route {

namespace {

constexpr char kAirLabsBase[] = "https://airlabs.co/api/v9/flight";
constexpr char kFlightAwareBase[] = "https://aeroapi.flightaware.com/aeroapi/flights/";
constexpr char kFr24Base[] =
    "https://fr24api.flightradar24.com/api/flight-summary/light";

constexpr size_t kCacheSize = 64;

struct RouteInfo {
  char airline[28];
  char origin[5];
  char dest[5];
};

struct CacheSlot {
  char callsign[9];
  RouteInfo route;
  ApiSource source;
  uint32_t cached_at_sec;
  /** True after live API waterfall ran (hit or miss) — do not query APIs again until TTL. */
  bool api_done;
};

CacheSlot s_cache[kCacheSize];

TaskHandle_t s_detail_task = nullptr;
char s_detail_selection_callsign[9] = "";
char s_detail_worker_callsign[9] = "";
volatile bool s_detail_requested = false;
volatile bool s_detail_busy = false;
volatile bool s_detail_ready = false;
char s_detail_pending_callsign[9] = "";
volatile bool s_detail_has_pending = false;
unsigned long s_detail_worker_start_ms = 0;
char s_detail_debounce_callsign[9] = "";
unsigned long s_detail_debounce_deadline_ms = 0;
bool s_detail_debounce_pending = false;
constexpr unsigned long kDetailEnrichDebounceMs = 400;
RouteInfo s_detail_result = {};
ApiSource s_detail_result_src = ApiSource::kNone;

enum class DetailStep : uint8_t {
  kIdle = 0,
  kCache,
  kAirLabs,
  kFlightAware,
  kFr24,
  kPrefix,
  kDone,
};

DetailStep s_detail_step = DetailStep::kIdle;
char s_detail_work_callsign[9] = "";
unsigned long s_detail_step_start_ms = 0;

constexpr size_t kMaxHttpPayloadBytes = 49152;

bool apiAvailable();

const char* stepTag(DetailStep step) {
  switch (step) {
    case DetailStep::kCache:
      return "cache";
    case DetailStep::kAirLabs:
      return "AL";
    case DetailStep::kFlightAware:
      return "FA";
    case DetailStep::kFr24:
      return "FR";
    case DetailStep::kPrefix:
      return "pfx";
    default:
      return "?";
  }
}

void logDetailStepBegin(DetailStep step, const char* callsign) {
  if (!config::kSerialTraceDebug) {
    return;
  }
  s_detail_step_start_ms = millis();
  Serial.printf("[detail] step %s %s begin\n", stepTag(step), callsign);
}

void logDetailStepEnd(DetailStep step, const char* callsign, bool ok) {
  if (!config::kSerialTraceDebug) {
    return;
  }
  const unsigned long elapsed =
      s_detail_step_start_ms > 0 ? millis() - s_detail_step_start_ms : 0;
  Serial.printf("[detail] step %s %s %s (%lums)\n", stepTag(step), callsign,
                ok ? "ok" : "miss", elapsed);
}

bool isCurrentDetailSelection(const char* callsign);

void workerYield() { vTaskDelay(1); }

bool readHttpPayload(HTTPClient& http, String* payload, const char* worker_callsign,
                     uint32_t timeout_ms) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr || payload == nullptr) {
    return false;
  }
  payload->clear();
  const int content_len = http.getSize();
  if (content_len > 0) {
    payload->reserve(static_cast<unsigned>(content_len));
  }
  const unsigned long deadline_ms = millis() + timeout_ms;
  uint8_t buf[512];
  while (http.connected() || stream->available()) {
    if (millis() >= deadline_ms) {
      if (config::kSerialTraceDebug) {
        Serial.printf("[detail] http read timeout (%ums)\n", timeout_ms);
      }
      return false;
    }
    if (worker_callsign != nullptr && !isCurrentDetailSelection(worker_callsign)) {
      if (config::kSerialTraceDebug) {
        Serial.printf("[detail] http read abort worker=%s\n", worker_callsign);
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
    if (payload->length() + n > kMaxHttpPayloadBytes) {
      if (config::kSerialTraceDebug) {
        Serial.printf("[detail] http read too large (>%u)\n",
                      static_cast<unsigned>(kMaxHttpPayloadBytes));
      }
      return false;
    }
    payload->concat(reinterpret_cast<const char*>(buf), n);
  }
  return payload->length() > 0;
}

void routeClear(RouteInfo* r) {
  if (r == nullptr) {
    return;
  }
  r->airline[0] = '\0';
  r->origin[0] = '\0';
  r->dest[0] = '\0';
}

bool routeHasData(const RouteInfo& r) {
  return r.airline[0] != '\0' || r.origin[0] != '\0' || r.dest[0] != '\0';
}

bool routeEndpointsComplete(const RouteInfo& r) {
  return r.origin[0] != '\0' && r.dest[0] != '\0';
}

int findCache(const char* callsign);

struct DetailPosition {
  double lat = 0.0;
  double lon = 0.0;
  int alt_ft = 0;
  bool valid = false;
};

DetailPosition loadDetailPosition(const char* callsign) {
  DetailPosition pos;
  services::adsb::Aircraft ac = {};
  if (!services::adsb::copyAircraftByCallsign(callsign, &ac)) {
    return pos;
  }
  pos.lat = static_cast<double>(ac.lat);
  pos.lon = static_cast<double>(ac.lon);
  pos.valid = true;
  int ft = 0;
  if (sscanf(ac.alt, "%d", &ft) == 1) {
    pos.alt_ft = ft;
  }
  return pos;
}

float airportDistKm(const char* code, const DetailPosition& pos) {
  float lat = 0.0f;
  float lon = 0.0f;
  if (!services::airport::lookupCoords(code, &lat, &lon)) {
    return -1.0f;
  }
  float dist_km = 0.0f;
  geo::localOffsetKm(pos.lat, pos.lon, lat, lon, nullptr, nullptr, &dist_km);
  return dist_km;
}

float jsonAirportDistKm(const JsonObject& airport, const DetailPosition& pos) {
  if (airport.isNull()) {
    return -1.0f;
  }
  if (!airport["latitude"].is<double>() && !airport["latitude"].is<float>() &&
      !airport["latitude"].is<int>()) {
    return -1.0f;
  }
  const float lat = airport["latitude"].as<float>();
  const float lon = airport["longitude"].as<float>();
  float dist_km = 0.0f;
  geo::localOffsetKm(pos.lat, pos.lon, lat, lon, nullptr, nullptr, &dist_km);
  return dist_km;
}

bool routePlausibleForPosition(const RouteInfo& route, const DetailPosition& pos) {
  if (!pos.valid || !routeEndpointsComplete(route)) {
    return true;
  }

  const float orig_km = airportDistKm(route.origin, pos);
  const float dest_km = airportDistKm(route.dest, pos);
  if (orig_km < 0.0f || dest_km < 0.0f) {
    return true;
  }

  constexpr float kNearKm = 75.0f;
  constexpr float kLongLegKm = 250.0f;
  constexpr int kCruiseFt = 8000;

  const bool near_orig = orig_km < kNearKm;
  const bool near_dest = dest_km < kNearKm;

  if (near_dest) {
    return true;
  }
  if (near_orig && dest_km > kLongLegKm && pos.alt_ft > kCruiseFt) {
    return false;
  }
  if (!near_orig && !near_dest && orig_km + 50.0f < dest_km) {
    return false;
  }
  return true;
}

void rejectCacheRam(const char* callsign) {
  const int idx = findCache(callsign);
  if (idx < 0) {
    return;
  }
  routeClear(&s_cache[idx].route);
  s_cache[idx].api_done = false;
  s_cache[idx].source = ApiSource::kNone;
}

bool slotNeedsApiRouteUpgrade(const CacheSlot& slot) {
  if (slot.callsign[0] == '\0') {
    return false;
  }
  if (slot.source == ApiSource::kPrefix) {
    return true;
  }
  if (routeEndpointsComplete(slot.route)) {
    return false;
  }
  if (slot.api_done && slot.source == ApiSource::kNone) {
    return false;
  }
  return true;
}

void applyRouteToAircraft(services::adsb::Aircraft& ac, const RouteInfo& info) {
  if (info.airline[0] != '\0') {
    strncpy(ac.airline, info.airline, sizeof(ac.airline) - 1);
    ac.airline[sizeof(ac.airline) - 1] = '\0';
  }
  if (info.origin[0] != '\0') {
    char resolved[5];
    if (services::airport::normalizeRouteCode(info.origin, resolved, sizeof(resolved))) {
      strncpy(ac.route_origin, resolved, sizeof(ac.route_origin) - 1);
    } else {
      strncpy(ac.route_origin, info.origin, sizeof(ac.route_origin) - 1);
    }
    ac.route_origin[sizeof(ac.route_origin) - 1] = '\0';
  }
  if (info.dest[0] != '\0') {
    char resolved[5];
    if (services::airport::normalizeRouteCode(info.dest, resolved, sizeof(resolved))) {
      strncpy(ac.route_dest, resolved, sizeof(ac.route_dest) - 1);
    } else {
      strncpy(ac.route_dest, info.dest, sizeof(ac.route_dest) - 1);
    }
    ac.route_dest[sizeof(ac.route_dest) - 1] = '\0';
  }
}

void copyAirportCode(const char* s, char* out, size_t out_len) {
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
    out[n] = static_cast<char>(islower(c) ? toupper(c) : s[n]);
    ++n;
  }
  out[n] = '\0';
}

void copyRouteIcao(const char* s, char* out, size_t out_len) {
  copyAirportCode(s, out, out_len);
  if (out[0] == '\0') {
    return;
  }
  char resolved[5];
  if (services::airport::normalizeRouteCode(out, resolved, sizeof(resolved))) {
    copyAirportCode(resolved, out, out_len);
  }
}

bool isIcaoRadioCallsign(const char* cs) {
  if (cs == nullptr || strlen(cs) < 4) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!isupper(static_cast<unsigned char>(cs[i]))) {
      return false;
    }
  }
  for (size_t i = 3; cs[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(cs[i]);
    if (!isupper(c) && !isdigit(c)) {
      return false;
    }
  }
  return true;
}

uint32_t nowSec() {
  const time_t t = time(nullptr);
  if (t < 1600000000) {
    return static_cast<uint32_t>(millis() / 1000U);
  }
  return static_cast<uint32_t>(t);
}

int findCache(const char* callsign) {
  for (size_t i = 0; i < kCacheSize; ++i) {
    if (s_cache[i].callsign[0] != '\0' && strcmp(s_cache[i].callsign, callsign) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void storeCache(const char* callsign, const RouteInfo& info, ApiSource src, bool api_done,
                uint32_t cached_at_sec, bool mark_flash_dirty) {
  int slot = findCache(callsign);
  if (slot < 0) {
    slot = -1;
    for (size_t i = 0; i < kCacheSize; ++i) {
      if (s_cache[i].callsign[0] == '\0') {
        slot = static_cast<int>(i);
        break;
      }
    }
    if (slot < 0) {
      slot = -1;
      uint32_t oldest_done = UINT32_MAX;
      for (size_t i = 0; i < kCacheSize; ++i) {
        if (!s_cache[i].api_done) {
          continue;
        }
        if (s_cache[i].cached_at_sec < oldest_done) {
          oldest_done = s_cache[i].cached_at_sec;
          slot = static_cast<int>(i);
        }
      }
    }
    if (slot < 0) {
      slot = 0;
      uint32_t oldest = UINT32_MAX;
      for (size_t i = 0; i < kCacheSize; ++i) {
        if (s_cache[i].cached_at_sec < oldest) {
          oldest = s_cache[i].cached_at_sec;
          slot = static_cast<int>(i);
        }
      }
    }
  }
  strncpy(s_cache[slot].callsign, callsign, sizeof(s_cache[slot].callsign) - 1);
  s_cache[slot].callsign[sizeof(s_cache[slot].callsign) - 1] = '\0';
  s_cache[slot].route = info;
  s_cache[slot].source = src;
  s_cache[slot].cached_at_sec = cached_at_sec;
  s_cache[slot].api_done = api_done;
  if (mark_flash_dirty) {
    route_cache::markDirty();
  }
}

void storeCache(const char* callsign, const RouteInfo& info, ApiSource src, bool api_done) {
  storeCache(callsign, info, src, api_done, nowSec(), true);
}

void loadFileEntryToRam(const char* callsign, const route_cache::Entry& file_entry) {
  RouteInfo info;
  routeClear(&info);
  strncpy(info.airline, file_entry.airline, sizeof(info.airline) - 1);
  info.airline[sizeof(info.airline) - 1] = '\0';
  strncpy(info.origin, file_entry.origin, sizeof(info.origin) - 1);
  info.origin[sizeof(info.origin) - 1] = '\0';
  strncpy(info.dest, file_entry.dest, sizeof(info.dest) - 1);
  info.dest[sizeof(info.dest) - 1] = '\0';
  const ApiSource src = static_cast<ApiSource>(file_entry.source);
  storeCache(callsign, info, src, file_entry.api_done, file_entry.cached_at_sec, false);
}

/**
 * Load RAM cache from flash if needed. Returns slot index or -1.
 */
int loadCacheSlotForCallsign(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return -1;
  }

  int idx = findCache(callsign);
  if (idx >= 0) {
    return idx;
  }

  route_cache::Entry file_entry;
  if (route_cache::lookupPermanent(callsign, &file_entry)) {
    loadFileEntryToRam(callsign, file_entry);
    return findCache(callsign);
  }

  const uint32_t now = nowSec();
  if (!route_cache::lookup(callsign, &file_entry, now, config::kRouteLookupCacheTtlSec)) {
    return -1;
  }

  loadFileEntryToRam(callsign, file_entry);
  return findCache(callsign);
}

/**
 * If cached route is complete (or APIs off), copy it and return true — skip live APIs.
 * Always fills out/src_out when cache data exists, even if a live upgrade is still needed.
 */
bool cacheResolve(const char* callsign, RouteInfo* out, ApiSource* src_out) {
  const int idx = loadCacheSlotForCallsign(callsign);
  if (idx < 0) {
    return false;
  }

  const CacheSlot& slot = s_cache[idx];
  if (out != nullptr) {
    *out = slot.route;
  }
  if (src_out != nullptr) {
    *src_out = slot.source;
  }

  if (apiAvailable()) {
    return !slotNeedsApiRouteUpgrade(slot);
  }

  return slot.api_done || routeHasData(slot.route) || slot.source == ApiSource::kPrefix;
}

/** RAM-only cache resolve for the detail worker (never blocks on LittleFS). */
bool cacheResolveRam(const char* callsign, RouteInfo* out, ApiSource* src_out) {
  const int idx = findCache(callsign);
  if (idx < 0) {
    return false;
  }

  const CacheSlot& slot = s_cache[idx];
  if (out != nullptr) {
    *out = slot.route;
  }
  if (src_out != nullptr) {
    *src_out = slot.source;
  }

  if (apiAvailable()) {
    return !slotNeedsApiRouteUpgrade(slot);
  }

  return slot.api_done || routeHasData(slot.route) || slot.source == ApiSource::kPrefix;
}

bool apiLookupAlreadyDoneRam(const char* callsign) {
  if (!apiAvailable()) {
    return true;
  }
  const int idx = findCache(callsign);
  if (idx < 0) {
    return false;
  }
  return !slotNeedsApiRouteUpgrade(s_cache[idx]);
}

bool readRamCacheSlot(size_t index, route_cache::Entry* out, size_t max_index) {
  if (out == nullptr || index >= max_index || index >= kCacheSize) {
    return false;
  }
  const CacheSlot& slot = s_cache[index];
  if (slot.callsign[0] == '\0') {
    return false;
  }
  strncpy(out->callsign, slot.callsign, sizeof(out->callsign) - 1);
  out->callsign[sizeof(out->callsign) - 1] = '\0';
  strncpy(out->airline, slot.route.airline, sizeof(out->airline) - 1);
  out->airline[sizeof(out->airline) - 1] = '\0';
  strncpy(out->origin, slot.route.origin, sizeof(out->origin) - 1);
  out->origin[sizeof(out->origin) - 1] = '\0';
  strncpy(out->dest, slot.route.dest, sizeof(out->dest) - 1);
  out->dest[sizeof(out->dest) - 1] = '\0';
  out->source = static_cast<uint8_t>(slot.source);
  out->cached_at_sec = slot.cached_at_sec;
  out->api_done = slot.api_done;
  return true;
}

void copyJsonAirlineName(const JsonObject& obj, const char* key, char* out, size_t out_len) {
  out[0] = '\0';
  if (!obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  if (s == nullptr || s[0] == '\0') {
    return;
  }
  strncpy(out, s, out_len - 1);
  out[out_len - 1] = '\0';
}

struct HttpHeader {
  const char* name;
  const char* value;
};

bool httpGetJson(const char* url, JsonDocument& doc, const char* worker_callsign,
                 uint32_t timeout_ms, const HttpHeader* headers, size_t header_count) {
  if (worker_callsign != nullptr && !isCurrentDetailSelection(worker_callsign)) {
    return false;
  }
  services::https::ScopedLock tls(timeout_ms);
  if (!tls.held()) {
    return false;
  }
  if (worker_callsign != nullptr && !isCurrentDetailSelection(worker_callsign)) {
    return false;
  }

  const unsigned long http_start_ms = millis();
  if (config::kSerialTraceDebug && worker_callsign != nullptr) {
    Serial.printf("[detail] http begin %s\n", worker_callsign);
  }

  WiFiClientSecure client;
  client.setInsecure();
  const uint32_t timeout_sec = (timeout_ms + 999U) / 1000U;
  client.setTimeout(timeout_sec);
  client.setHandshakeTimeout(timeout_sec);
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  for (size_t i = 0; i < header_count; ++i) {
    if (headers[i].name != nullptr && headers[i].value != nullptr) {
      http.addHeader(headers[i].name, headers[i].value);
    }
  }
  http.setConnectTimeout(timeout_ms);
  http.setTimeout(timeout_ms);
  http.setReuse(false);
  const int http_code = http.sendRequest("GET");
  if (config::kSerialTraceDebug && worker_callsign != nullptr) {
    Serial.printf("[detail] http rsp %s code=%d (%lums)\n", worker_callsign, http_code,
                  millis() - http_start_ms);
  }
  if (http_code != HTTP_CODE_OK) {
    http.end();
    client.stop();
    return false;
  }
  if (worker_callsign != nullptr && !isCurrentDetailSelection(worker_callsign)) {
    http.end();
    client.stop();
    return false;
  }
  String payload;
  if (!readHttpPayload(http, &payload, worker_callsign, timeout_ms)) {
    http.end();
    client.stop();
    return false;
  }
  http.end();
  client.stop();
  workerYield();
  if (worker_callsign != nullptr && !isCurrentDetailSelection(worker_callsign)) {
    return false;
  }
  return !deserializeJson(doc, payload);
}

void fillAirlineFromAirLabs(const JsonObject& data, RouteInfo* route) {
  copyJsonAirlineName(data, "airline_name", route->airline, sizeof(route->airline));
  if (route->airline[0] != '\0') {
    return;
  }

  char code[4];
  const char* cs_iata = data["cs_airline_iata"].as<const char*>();
  if (cs_iata != nullptr && cs_iata[0] != '\0') {
    strncpy(code, cs_iata, sizeof(code) - 1);
    code[sizeof(code) - 1] = '\0';
    if (services::airline::lookupByCode(code, route->airline, sizeof(route->airline))) {
      return;
    }
  }

  const char* flight_iata = data["flight_iata"].as<const char*>();
  if (flight_iata != nullptr && strlen(flight_iata) >= 2) {
    memcpy(code, flight_iata, 2);
    code[2] = '\0';
    if (services::airline::lookupByCode(code, route->airline, sizeof(route->airline))) {
      return;
    }
  }

  const char* airline_icao = data["airline_icao"].as<const char*>();
  if (airline_icao != nullptr && strlen(airline_icao) == 3) {
    services::airline::lookupByCode(airline_icao, route->airline, sizeof(route->airline));
  }
}

bool apiAvailable() {
  return (apikeys::useAirLabs() && apikeys::hasAirLabs() && apikeys::canUseAirLabs()) ||
         (apikeys::useFlightAware() && apikeys::hasFlightAware() &&
          apikeys::canUseFlightAware()) ||
         (apikeys::useFr24() && apikeys::hasFr24() && apikeys::canUseFr24());
}

DetailStep firstLiveApiStep() {
  if (apikeys::useFlightAware() && apikeys::hasFlightAware() &&
      apikeys::canUseFlightAware()) {
    return DetailStep::kFlightAware;
  }
  if (apikeys::useAirLabs() && apikeys::hasAirLabs() && apikeys::canUseAirLabs()) {
    return DetailStep::kAirLabs;
  }
  if (apikeys::useFr24() && apikeys::hasFr24() && apikeys::canUseFr24()) {
    return DetailStep::kFr24;
  }
  return DetailStep::kPrefix;
}

DetailStep nextApiStepAfter(DetailStep step) {
  switch (step) {
    case DetailStep::kFlightAware:
      if (apikeys::useAirLabs() && apikeys::hasAirLabs() && apikeys::canUseAirLabs()) {
        return DetailStep::kAirLabs;
      }
      if (apikeys::useFr24() && apikeys::hasFr24() && apikeys::canUseFr24()) {
        return DetailStep::kFr24;
      }
      return DetailStep::kPrefix;
    case DetailStep::kAirLabs:
      if (apikeys::useFr24() && apikeys::hasFr24() && apikeys::canUseFr24()) {
        return DetailStep::kFr24;
      }
      return DetailStep::kPrefix;
    case DetailStep::kFr24:
    default:
      return DetailStep::kPrefix;
  }
}

bool lookupAirLabsWithKey(const char* callsign, RouteInfo* route, const char* api_key,
                           size_t key_index) {
  if (api_key == nullptr || api_key[0] == '\0' || route == nullptr) {
    return false;
  }
  routeClear(route);

  String url = kAirLabsBase;
  url += "?flight_icao=";
  url += callsign;
  url += "&api_key=";
  url += api_key;

  JsonDocument doc;
  if (!httpGetJson(url.c_str(), doc, callsign, config::kDetailApiTimeoutMs, nullptr, 0)) {
    return false;
  }

  // AirLabs returns an error object like:
  // { "error": { "code": "month_limit_exceeded", ... } }
  // Mark only the failing key exhausted and keep trying the next one.
  if (doc["error"].is<JsonObject>()) {
    const char* err_code = doc["error"]["code"].as<const char*>();
    if (err_code != nullptr && strcmp(err_code, "month_limit_exceeded") == 0) {
      apikeys::exhaustAirLabsKeyAt(key_index);
      return false;
    }
  }

  JsonObject data = doc["response"].as<JsonObject>();
  if (data.isNull()) {
    return false;
  }

  copyRouteIcao(data["dep_icao"].as<const char*>(), route->origin, sizeof(route->origin));
  if (route->origin[0] == '\0') {
    copyRouteIcao(data["dep_iata"].as<const char*>(), route->origin, sizeof(route->origin));
  }
  copyRouteIcao(data["arr_icao"].as<const char*>(), route->dest, sizeof(route->dest));
  if (route->dest[0] == '\0') {
    copyRouteIcao(data["arr_iata"].as<const char*>(), route->dest, sizeof(route->dest));
  }
  fillAirlineFromAirLabs(data, route);
  return routeHasData(*route);
}

bool lookupAirLabsFirstKey(const char* callsign, RouteInfo* route) {
  if (!apikeys::useAirLabs() || !apikeys::hasAirLabs() || route == nullptr) {
    return false;
  }
  if (!isCurrentDetailSelection(callsign)) {
    return false;
  }
  const size_t key_count = apikeys::airLabsKeyCount();
  for (size_t i = 0; i < key_count; ++i) {
    if (!apikeys::canUseAirLabsAt(i)) {
      continue;
    }
    const bool ok =
        lookupAirLabsWithKey(callsign, route, apikeys::airLabsKeyAt(i), i);
    apikeys::recordAirLabsCallAt(i);
    return ok;
  }
  return false;
}

bool pickFlightAwareFlight(JsonArray flights, JsonObject* chosen, const DetailPosition& pos) {
  float best_score = -1.0f;
  JsonObject best_flight;

  for (JsonObject f : flights) {
    if (f["origin"].isNull() || f["destination"].isNull()) {
      continue;
    }

    RouteInfo candidate;
    routeClear(&candidate);
    JsonObject origin = f["origin"].as<JsonObject>();
    JsonObject dest = f["destination"].as<JsonObject>();
    copyRouteIcao(origin["code_icao"].as<const char*>(), candidate.origin,
                    sizeof(candidate.origin));
    if (candidate.origin[0] == '\0') {
      copyRouteIcao(origin["code_iata"].as<const char*>(), candidate.origin,
                      sizeof(candidate.origin));
    }
    copyRouteIcao(dest["code_icao"].as<const char*>(), candidate.dest, sizeof(candidate.dest));
    if (candidate.dest[0] == '\0') {
      copyRouteIcao(dest["code_iata"].as<const char*>(), candidate.dest, sizeof(candidate.dest));
    }
    if (!routeEndpointsComplete(candidate)) {
      continue;
    }
    if (!routePlausibleForPosition(candidate, pos)) {
      continue;
    }

    float score = 0.0f;
    const char* status = f["status"].as<const char*>();
    if (status != nullptr && strcmp(status, "En Route") == 0) {
      score += 50.0f;
    }
    const float dest_km = jsonAirportDistKm(dest, pos);
    const float orig_km = jsonAirportDistKm(origin, pos);
    if (dest_km >= 0.0f) {
      score += fmaxf(0.0f, 120.0f - dest_km);
    }
    if (orig_km >= 0.0f) {
      score -= fmaxf(0.0f, 80.0f - orig_km) * 0.5f;
    }

    if (score > best_score) {
      best_score = score;
      best_flight = f;
    }
  }

  if (best_score < 0.0f) {
    return false;
  }
  *chosen = best_flight;
  return true;
}

bool lookupFlightAwareWithKey(const char* callsign, RouteInfo* route, const char* api_key,
                              const DetailPosition& pos) {
  if (api_key == nullptr || api_key[0] == '\0' || route == nullptr) {
    return false;
  }
  routeClear(route);

  String url = kFlightAwareBase;
  url += callsign;
  url += "?max_pages=1";

  const HttpHeader headers[] = {{"x-apikey", api_key}};
  JsonDocument doc;
  if (!httpGetJson(url.c_str(), doc, callsign, config::kDetailApiTimeoutMs, headers, 1)) {
    return false;
  }

  JsonArray flights = doc["flights"].as<JsonArray>();
  if (flights.isNull() || flights.size() == 0) {
    return false;
  }

  JsonObject flight;
  if (!pickFlightAwareFlight(flights, &flight, pos)) {
    return false;
  }

  JsonObject origin = flight["origin"].as<JsonObject>();
  JsonObject dest = flight["destination"].as<JsonObject>();
  if (!origin.isNull()) {
    copyRouteIcao(origin["code_icao"].as<const char*>(), route->origin,
                    sizeof(route->origin));
    if (route->origin[0] == '\0') {
      copyRouteIcao(origin["code_iata"].as<const char*>(), route->origin,
                      sizeof(route->origin));
    }
  }
  if (!dest.isNull()) {
    copyRouteIcao(dest["code_icao"].as<const char*>(), route->dest, sizeof(route->dest));
    if (route->dest[0] == '\0') {
      copyRouteIcao(dest["code_iata"].as<const char*>(), route->dest, sizeof(route->dest));
    }
  }

  copyJsonAirlineName(flight, "operator", route->airline, sizeof(route->airline));
  if (route->airline[0] == '\0') {
    char code[4];
    const char* op_iata = flight["operator_iata"].as<const char*>();
    if (op_iata != nullptr && op_iata[0] != '\0') {
      strncpy(code, op_iata, sizeof(code) - 1);
      code[sizeof(code) - 1] = '\0';
      services::airline::lookupByCode(code, route->airline, sizeof(route->airline));
    }
    if (route->airline[0] == '\0') {
      const char* op_icao = flight["operator_icao"].as<const char*>();
      if (op_icao != nullptr && strlen(op_icao) == 3) {
        services::airline::lookupByCode(op_icao, route->airline, sizeof(route->airline));
      }
    }
  }
  return routeHasData(*route);
}

bool lookupFlightAwareFirstKey(const char* callsign, RouteInfo* route,
                               const DetailPosition& pos) {
  if (!apikeys::useFlightAware() || !apikeys::hasFlightAware() || route == nullptr) {
    return false;
  }
  if (!isCurrentDetailSelection(callsign)) {
    return false;
  }
  const size_t key_count = apikeys::flightAwareKeyCount();
  for (size_t i = 0; i < key_count; ++i) {
    if (!apikeys::canUseFlightAwareAt(i)) {
      continue;
    }
    const bool ok =
        lookupFlightAwareWithKey(callsign, route, apikeys::flightAwareKeyAt(i), pos);
    apikeys::recordFlightAwareCallAt(i);
    return ok;
  }
  return false;
}

/** FR24 query timestamps: `YYYY-MM-DDTHH:MM:SS` (20 chars + NUL). */
constexpr size_t kIsoUtcLen = 21;

void formatIsoUtc(time_t t, char* buf, size_t len) {
  if (buf == nullptr || len < kIsoUtcLen) {
    return;
  }
  struct tm tm_utc;
  if (gmtime_r(&t, &tm_utc) == nullptr) {
    buf[0] = '\0';
    return;
  }
  strftime(buf, kIsoUtcLen, "%Y-%m-%dT%H:%M:%S", &tm_utc);
}

bool lookupFr24WithKey(const char* callsign, RouteInfo* route, const char* api_token) {
  if (api_token == nullptr || api_token[0] == '\0' || route == nullptr) {
    return false;
  }
  routeClear(route);

  time_t now = time(nullptr);
  if (now < 1600000000) {
    return false;
  }
  char from_iso[kIsoUtcLen];
  char to_iso[kIsoUtcLen];
  formatIsoUtc(now - 12 * 3600, from_iso, sizeof(from_iso));
  formatIsoUtc(now + 2 * 3600, to_iso, sizeof(to_iso));

  String url = kFr24Base;
  url += "?callsigns=";
  url += callsign;
  url += "&flight_datetime_from=";
  url += from_iso;
  url += "&flight_datetime_to=";
  url += to_iso;
  url += "&limit=5&sort=desc";

  String auth = "Bearer ";
  auth += api_token;
  const HttpHeader headers[] = {{"Accept", "application/json"},
                                {"Accept-Version", "v1"},
                                {"Authorization", auth.c_str()}};
  JsonDocument doc;
  if (!httpGetJson(url.c_str(), doc, callsign, config::kDetailApiTimeoutMs, headers, 3)) {
    return false;
  }

  JsonArray data = doc["data"].as<JsonArray>();
  if (data.isNull() || data.size() == 0) {
    return false;
  }

  JsonObject f = data[0].as<JsonObject>();
  for (JsonObject cand : data) {
    if (cand["flight_ended"].is<bool>() && !cand["flight_ended"].as<bool>()) {
      f = cand;
      break;
    }
  }

  copyRouteIcao(f["orig_icao"].as<const char*>(), route->origin, sizeof(route->origin));
  const char* dest_icao = f["dest_icao_actual"].as<const char*>();
  if (dest_icao == nullptr || dest_icao[0] == '\0') {
    dest_icao = f["dest_icao"].as<const char*>();
  }
  copyRouteIcao(dest_icao, route->dest, sizeof(route->dest));

  const char* painted = f["painted_as"].as<const char*>();
  const char* operating = f["operating_as"].as<const char*>();
  if (operating == nullptr || operating[0] == '\0') {
    operating = f["operated_as"].as<const char*>();
  }

  if (painted != nullptr && strlen(painted) == 3) {
    services::airline::lookupByCode(painted, route->airline, sizeof(route->airline));
  }
  if (route->airline[0] == '\0' && operating != nullptr && strlen(operating) == 3) {
    services::airline::lookupByCode(operating, route->airline, sizeof(route->airline));
  }
  if (route->airline[0] == '\0' && painted != nullptr && painted[0] != '\0') {
    strncpy(route->airline, painted, sizeof(route->airline) - 1);
    route->airline[sizeof(route->airline) - 1] = '\0';
  }
  return routeHasData(*route);
}

bool lookupFr24FirstKey(const char* callsign, RouteInfo* route) {
  if (!apikeys::useFr24() || !apikeys::hasFr24() || route == nullptr) {
    return false;
  }
  if (!isCurrentDetailSelection(callsign)) {
    return false;
  }
  const size_t key_count = apikeys::fr24KeyCount();
  for (size_t i = 0; i < key_count; ++i) {
    if (!apikeys::canUseFr24At(i)) {
      continue;
    }
    const bool ok = lookupFr24WithKey(callsign, route, apikeys::fr24KeyAt(i));
    apikeys::recordFr24CallAt(i);
    return ok;
  }
  return false;
}

bool lookupPrefixFallback(const char* callsign, RouteInfo* route) {
  if (route == nullptr) {
    return false;
  }
  routeClear(route);
  services::airline::resolveFromCallsign(callsign, true, route->airline, sizeof(route->airline));
  return route->airline[0] != '\0';
}

void logRouteLine(const char* callsign, const RouteInfo& route, const char* tag) {
  char leg[12];
  leg[0] = '\0';
  if (route.origin[0] != '\0' && route.dest[0] != '\0') {
    snprintf(leg, sizeof(leg), "%s→%s", route.origin, route.dest);
  } else if (route.origin[0] != '\0') {
    snprintf(leg, sizeof(leg), "%s→?", route.origin);
  } else if (route.dest[0] != '\0') {
    snprintf(leg, sizeof(leg), "?→%s", route.dest);
  } else {
    strncpy(leg, "—", sizeof(leg) - 1);
    leg[sizeof(leg) - 1] = '\0';
  }
  Serial.printf("[route] %s -> %s %s [%s]\n", callsign,
                route.airline[0] != '\0' ? route.airline : "(no airline)", leg, tag);
}

void applyRouteToCallsign(const char* callsign, const RouteInfo& info) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return;
  }
  services::adsb::applyRouteFieldsByCallsign(callsign, info.airline, info.origin, info.dest);
}

bool isCurrentDetailSelection(const char* callsign) {
  return callsign != nullptr && callsign[0] != '\0' &&
         strcmp(callsign, s_detail_selection_callsign) == 0;
}

void signalDetailReadyIfSelected(const char* callsign) {
  if (isCurrentDetailSelection(callsign)) {
    s_detail_ready = true;
  }
}

void clearDetailDebounce() {
  s_detail_debounce_pending = false;
  s_detail_debounce_deadline_ms = 0;
  s_detail_debounce_callsign[0] = '\0';
}

void scheduleDetailEnrichDebounce(const char* callsign, unsigned long now_ms) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return;
  }
  strncpy(s_detail_debounce_callsign, callsign, sizeof(s_detail_debounce_callsign) - 1);
  s_detail_debounce_callsign[sizeof(s_detail_debounce_callsign) - 1] = '\0';
  s_detail_debounce_deadline_ms = now_ms + kDetailEnrichDebounceMs;
  s_detail_debounce_pending = true;
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] debounce %s %lums\n", callsign, kDetailEnrichDebounceMs);
  }
}

/** Cache-only path; returns true when no live API worker is needed. */
bool tryDetailCacheOnly(const char* callsign) {
  RouteInfo cached;
  routeClear(&cached);
  ApiSource cached_src = ApiSource::kNone;
  const bool cache_complete = cacheResolveRam(callsign, &cached, &cached_src);
  const DetailPosition pos = loadDetailPosition(callsign);
  if (routeHasData(cached) && !routePlausibleForPosition(cached, pos)) {
    rejectCacheRam(callsign);
    return false;
  }
  if (routeHasData(cached)) {
    applyRouteToCallsign(callsign, cached);
  }
  if (cache_complete || apiLookupAlreadyDoneRam(callsign)) {
    if (routeHasData(cached) && isCurrentDetailSelection(callsign)) {
      logRouteLine(callsign, cached, "cache");
    }
    return true;
  }
  return false;
}

void setDetailPending(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return;
  }
  strncpy(s_detail_pending_callsign, callsign, sizeof(s_detail_pending_callsign) - 1);
  s_detail_pending_callsign[sizeof(s_detail_pending_callsign) - 1] = '\0';
  s_detail_has_pending = true;
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] pending %s (worker busy)\n", callsign);
  }
}

void startDetailWorker(const char* callsign) {
  strncpy(s_detail_worker_callsign, callsign, sizeof(s_detail_worker_callsign) - 1);
  s_detail_worker_callsign[sizeof(s_detail_worker_callsign) - 1] = '\0';
  s_detail_ready = false;
  s_detail_requested = true;
  Serial.printf("Route lookup: detail enrich %s\n", callsign);
}

void drainDetailPending() {
  if (!s_detail_has_pending || s_detail_pending_callsign[0] == '\0') {
    return;
  }

  if (!isCurrentDetailSelection(s_detail_pending_callsign)) {
    s_detail_has_pending = false;
    s_detail_pending_callsign[0] = '\0';
    return;
  }

  char callsign[sizeof(s_detail_pending_callsign)];
  strncpy(callsign, s_detail_pending_callsign, sizeof(callsign));
  callsign[sizeof(callsign) - 1] = '\0';
  s_detail_has_pending = false;
  s_detail_pending_callsign[0] = '\0';

  if (tryDetailCacheOnly(callsign)) {
    return;
  }
  if (s_detail_busy || s_detail_requested) {
    setDetailPending(callsign);
    return;
  }
  startDetailWorker(callsign);
}

void detailWorkerCancelWork() {
  if (config::kSerialTraceDebug && s_detail_work_callsign[0] != '\0') {
    Serial.printf("[detail] worker cancel %s\n", s_detail_work_callsign);
  }
  s_detail_step = DetailStep::kIdle;
  s_detail_busy = false;
  s_detail_worker_start_ms = 0;
  s_detail_work_callsign[0] = '\0';
  routeClear(&s_detail_result);
  s_detail_result_src = ApiSource::kNone;
}

/** Returns true when work was cancelled (selection cleared). */
bool detailWorkerRetargetIfNeeded() {
  if (s_detail_selection_callsign[0] == '\0') {
    detailWorkerCancelWork();
    return true;
  }
  if (isCurrentDetailSelection(s_detail_work_callsign)) {
    return false;
  }
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] worker retarget %s -> %s\n", s_detail_work_callsign,
                  s_detail_selection_callsign);
  }
  strncpy(s_detail_work_callsign, s_detail_selection_callsign, sizeof(s_detail_work_callsign) - 1);
  s_detail_work_callsign[sizeof(s_detail_work_callsign) - 1] = '\0';
  strncpy(s_detail_worker_callsign, s_detail_selection_callsign,
          sizeof(s_detail_worker_callsign) - 1);
  s_detail_worker_callsign[sizeof(s_detail_worker_callsign) - 1] = '\0';
  s_detail_worker_start_ms = millis();
  s_detail_step = DetailStep::kCache;
  routeClear(&s_detail_result);
  s_detail_result_src = ApiSource::kNone;
  s_detail_ready = false;
  if (s_detail_has_pending &&
      strcmp(s_detail_pending_callsign, s_detail_selection_callsign) == 0) {
    s_detail_has_pending = false;
    s_detail_pending_callsign[0] = '\0';
  }
  return false;
}

void detailWorkerFinishJob() {
  const char* callsign = s_detail_work_callsign;
  const unsigned long elapsed =
      s_detail_worker_start_ms > 0 ? millis() - s_detail_worker_start_ms : 0;
  const bool still_selected = isCurrentDetailSelection(callsign);
  if (still_selected) {
    s_detail_ready = true;
  }
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] worker done %s (%lums) still_selected=%d\n", callsign, elapsed,
                  still_selected ? 1 : 0);
  }
  s_detail_step = DetailStep::kIdle;
  s_detail_busy = false;
  s_detail_worker_start_ms = 0;
  s_detail_work_callsign[0] = '\0';
  drainDetailPending();
}

void detailWorkerRunStep() {
  const char* callsign = s_detail_work_callsign;
  if (callsign[0] == '\0') {
    detailWorkerCancelWork();
    return;
  }

  switch (s_detail_step) {
    case DetailStep::kCache: {
      logDetailStepBegin(DetailStep::kCache, callsign);
      RouteInfo cached;
      routeClear(&cached);
      ApiSource cached_src = ApiSource::kNone;
      const DetailPosition pos = loadDetailPosition(callsign);
      bool cache_complete = cacheResolveRam(callsign, &cached, &cached_src);
      if (routeHasData(cached) && !routePlausibleForPosition(cached, pos)) {
        if (config::kSerialTraceDebug) {
          Serial.printf("[route] %s cache position reject %s→%s\n", callsign, cached.origin,
                        cached.dest);
        }
        rejectCacheRam(callsign);
        routeClear(&cached);
        cache_complete = false;
      }
      if (routeHasData(cached)) {
        s_detail_result = cached;
        s_detail_result_src = cached_src;
        applyRouteToCallsign(callsign, cached);
      }
      if (cache_complete || apiLookupAlreadyDoneRam(callsign)) {
        if (routeHasData(s_detail_result) && isCurrentDetailSelection(callsign)) {
          logRouteLine(callsign, s_detail_result, "cache");
        }
        logDetailStepEnd(DetailStep::kCache, callsign, true);
        s_detail_step = DetailStep::kDone;
        return;
      }
      logDetailStepEnd(DetailStep::kCache, callsign, false);
      s_detail_step = apiAvailable() ? firstLiveApiStep() : DetailStep::kPrefix;
      return;
    }
    case DetailStep::kAirLabs: {
      logDetailStepBegin(DetailStep::kAirLabs, callsign);
      RouteInfo live;
      routeClear(&live);
      bool ok = false;
      const DetailPosition pos = loadDetailPosition(callsign);
      if (apikeys::useAirLabs() && apikeys::hasAirLabs() && apikeys::canUseAirLabs() &&
          lookupAirLabsFirstKey(callsign, &live)) {
        if (routePlausibleForPosition(live, pos)) {
          s_detail_result = live;
          s_detail_result_src = ApiSource::kAirLabs;
          storeCache(callsign, live, ApiSource::kAirLabs, true);
          applyRouteToCallsign(callsign, live);
          if (isCurrentDetailSelection(callsign)) {
            logRouteLine(callsign, live, "AL");
          }
          ok = true;
          s_detail_step = DetailStep::kDone;
        } else if (config::kSerialTraceDebug) {
          Serial.printf("[route] %s AL position reject %s→%s\n", callsign, live.origin,
                        live.dest);
        }
      }
      if (!ok) {
        s_detail_step = nextApiStepAfter(DetailStep::kAirLabs);
      }
      logDetailStepEnd(DetailStep::kAirLabs, callsign, ok);
      return;
    }
    case DetailStep::kFlightAware: {
      logDetailStepBegin(DetailStep::kFlightAware, callsign);
      RouteInfo live;
      routeClear(&live);
      bool ok = false;
      const DetailPosition pos = loadDetailPosition(callsign);
      if (apikeys::useFlightAware() && apikeys::hasFlightAware() &&
          apikeys::canUseFlightAware() && lookupFlightAwareFirstKey(callsign, &live, pos)) {
        s_detail_result = live;
        s_detail_result_src = ApiSource::kFlightAware;
        storeCache(callsign, live, ApiSource::kFlightAware, true);
        applyRouteToCallsign(callsign, live);
        if (isCurrentDetailSelection(callsign)) {
          logRouteLine(callsign, live, "FA");
        }
        ok = true;
        s_detail_step = DetailStep::kDone;
      } else {
        s_detail_step = nextApiStepAfter(DetailStep::kFlightAware);
      }
      logDetailStepEnd(DetailStep::kFlightAware, callsign, ok);
      return;
    }
    case DetailStep::kFr24: {
      logDetailStepBegin(DetailStep::kFr24, callsign);
      RouteInfo live;
      routeClear(&live);
      bool ok = false;
      if (apikeys::useFr24() && apikeys::hasFr24() && apikeys::canUseFr24() &&
          lookupFr24FirstKey(callsign, &live)) {
        s_detail_result = live;
        s_detail_result_src = ApiSource::kFr24;
        storeCache(callsign, live, ApiSource::kFr24, true);
        applyRouteToCallsign(callsign, live);
        if (isCurrentDetailSelection(callsign)) {
          logRouteLine(callsign, live, "FR");
        }
        ok = true;
        s_detail_step = DetailStep::kDone;
      } else {
        RouteInfo miss;
        routeClear(&miss);
        storeCache(callsign, miss, ApiSource::kNone, true);
        s_detail_step = DetailStep::kPrefix;
      }
      logDetailStepEnd(DetailStep::kFr24, callsign, ok);
      return;
    }
    case DetailStep::kPrefix: {
      logDetailStepBegin(DetailStep::kPrefix, callsign);
      bool ok = false;
      if (lookupPrefixFallback(callsign, &s_detail_result)) {
        s_detail_result_src = ApiSource::kPrefix;
        storeCache(callsign, s_detail_result, ApiSource::kPrefix, true);
        if (isCurrentDetailSelection(callsign)) {
          logRouteLine(callsign, s_detail_result, "pfx");
        }
        ok = true;
      } else {
        RouteInfo miss;
        routeClear(&miss);
        storeCache(callsign, miss, ApiSource::kNone, true);
      }
      logDetailStepEnd(DetailStep::kPrefix, callsign, ok);
      s_detail_step = DetailStep::kDone;
      return;
    }
    case DetailStep::kDone:
      detailWorkerFinishJob();
      return;
    default:
      detailWorkerCancelWork();
      return;
  }
}

void detailWorkerTask(void* /*arg*/) {
  for (;;) {
    if (s_detail_requested && s_detail_step == DetailStep::kIdle) {
      s_detail_busy = true;
      s_detail_worker_start_ms = millis();
      strncpy(s_detail_work_callsign, s_detail_worker_callsign, sizeof(s_detail_work_callsign) - 1);
      s_detail_work_callsign[sizeof(s_detail_work_callsign) - 1] = '\0';
      s_detail_requested = false;
      s_detail_ready = false;
      routeClear(&s_detail_result);
      s_detail_result_src = ApiSource::kNone;
      s_detail_step = DetailStep::kCache;
      if (config::kSerialTraceDebug) {
        Serial.printf("[detail] worker begin %s\n", s_detail_work_callsign);
      }
    }

    if (s_detail_step != DetailStep::kIdle) {
      if (!detailWorkerRetargetIfNeeded()) {
        detailWorkerRunStep();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void ensureDetailWorker() {
  if (s_detail_task != nullptr) {
    return;
  }
  xTaskCreatePinnedToCore(detailWorkerTask, "route_detail", 16384, nullptr, 1,
                          &s_detail_task, 1);
}

void queueDetailEnrichment(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0' || !isIcaoRadioCallsign(callsign)) {
    return;
  }

  ensureDetailWorker();
  if (tryDetailCacheOnly(callsign)) {
    if (config::kSerialTraceDebug) {
      Serial.printf("[detail] enrich cache hit %s (skip worker)\n", callsign);
    }
    return;
  }
  if (s_detail_busy || s_detail_requested) {
    if (config::kSerialTraceDebug) {
      Serial.printf("[detail] queue busy -> pending %s\n", callsign);
    }
    setDetailPending(callsign);
    return;
  }

  startDetailWorker(callsign);
}

void onFlightDetailSelectedImpl(const char* callsign, const bool immediate) {
  if (callsign == nullptr || callsign[0] == '\0') {
    s_detail_selection_callsign[0] = '\0';
    clearDetailDebounce();
    return;
  }

  const bool selection_changed = strcmp(callsign, s_detail_selection_callsign) != 0;
  if (selection_changed) {
    strncpy(s_detail_selection_callsign, callsign, sizeof(s_detail_selection_callsign) - 1);
    s_detail_selection_callsign[sizeof(s_detail_selection_callsign) - 1] = '\0';
    s_detail_ready = false;
    if ((s_detail_busy || s_detail_requested) &&
        strcmp(callsign, s_detail_worker_callsign) != 0) {
      setDetailPending(callsign);
      if (config::kSerialTraceDebug) {
        Serial.printf("[detail] bump pending %s (worker on %s)\n", callsign,
                      s_detail_worker_callsign);
      }
    }
  } else if (!immediate) {
    return;
  }

  if (immediate) {
    if (config::kSerialTraceDebug) {
      Serial.printf("[detail] selected %s immediate\n", callsign);
    }
    clearDetailDebounce();
    queueDetailEnrichment(callsign);
    return;
  }

  if (config::kSerialTraceDebug && selection_changed) {
    Serial.printf("[detail] selected %s debounced\n", callsign);
  }
  scheduleDetailEnrichDebounce(callsign, millis());
}

void tickDetailEnrichDebounceImpl(unsigned long now_ms) {
  if (!s_detail_debounce_pending || s_detail_debounce_callsign[0] == '\0') {
    return;
  }
  if (now_ms < s_detail_debounce_deadline_ms) {
    return;
  }
  if (!isCurrentDetailSelection(s_detail_debounce_callsign)) {
    clearDetailDebounce();
    return;
  }

  char callsign[sizeof(s_detail_debounce_callsign)];
  strncpy(callsign, s_detail_debounce_callsign, sizeof(callsign));
  callsign[sizeof(callsign) - 1] = '\0';
  clearDetailDebounce();
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] debounce fire %s\n", callsign);
  }
  queueDetailEnrichment(callsign);
}

void cancelDetailEnrichmentImpl() {
  s_detail_selection_callsign[0] = '\0';
  s_detail_ready = false;
  s_detail_has_pending = false;
  s_detail_pending_callsign[0] = '\0';
  clearDetailDebounce();
}

bool detailEnrichmentReadyImpl() { return s_detail_ready; }

bool detailEnrichmentConsumeImpl() {
  if (!s_detail_ready) {
    return false;
  }

  const char* callsign = s_detail_selection_callsign;
  if (callsign[0] == '\0') {
    s_detail_ready = false;
    return false;
  }

  if (strcmp(callsign, s_detail_worker_callsign) != 0) {
    s_detail_ready = false;
    return false;
  }

  if (routeHasData(s_detail_result)) {
    services::adsb::Aircraft ac = {};
    if (services::adsb::copyAircraftByCallsign(callsign, &ac)) {
      char want_origin[5] = {};
      char want_dest[5] = {};
      if (services::airport::normalizeRouteCode(s_detail_result.origin, want_origin,
                                                sizeof(want_origin))) {
      } else {
        strncpy(want_origin, s_detail_result.origin, sizeof(want_origin) - 1);
      }
      if (services::airport::normalizeRouteCode(s_detail_result.dest, want_dest,
                                                sizeof(want_dest))) {
      } else {
        strncpy(want_dest, s_detail_result.dest, sizeof(want_dest) - 1);
      }
      const bool route_same =
          strcmp(ac.route_origin, want_origin) == 0 && strcmp(ac.route_dest, want_dest) == 0 &&
          (s_detail_result.airline[0] == '\0' ||
           strcmp(ac.airline, s_detail_result.airline) == 0);
      if (route_same) {
        s_detail_ready = false;
        return false;
      }
    }
    applyRouteToCallsign(callsign, s_detail_result);
  }

  s_detail_ready = false;
  return true;
}

void tickDetailWorkerWatchdogImpl(unsigned long now_ms) {
  if (!s_detail_busy || s_detail_worker_start_ms == 0) {
    return;
  }

  const unsigned long busy_ms = now_ms - s_detail_worker_start_ms;
  const bool stale = s_detail_work_callsign[0] != '\0' &&
                     !isCurrentDetailSelection(s_detail_work_callsign);
  const unsigned long warn_ms =
      stale ? config::kDetailWorkerStaleStallMs : config::kDetailWorkerStallMs;
  if (busy_ms < warn_ms) {
    return;
  }

  static unsigned long s_last_slow_log_ms = 0;
  if (now_ms - s_last_slow_log_ms < 5000UL) {
    return;
  }
  s_last_slow_log_ms = now_ms;

  if (stale) {
    Serial.printf("[detail] worker slow stale %s sel=%s step=%s (%lums)\n",
                  s_detail_work_callsign, s_detail_selection_callsign, stepTag(s_detail_step),
                  busy_ms);
    if (s_detail_selection_callsign[0] != '\0' &&
        strcmp(s_detail_selection_callsign, s_detail_work_callsign) != 0) {
      setDetailPending(s_detail_selection_callsign);
    }
    return;
  }

  Serial.printf("[detail] worker slow %s step=%s (%lums)\n", s_detail_work_callsign,
                stepTag(s_detail_step), busy_ms);
}

}  // namespace

void init() {
  services::https::init();
  apikeys::load();
  memset(s_cache, 0, sizeof(s_cache));
  if (route_cache::mount()) {
    Serial.println("Route lookup: flash cache mounted (/route_cache.csv)");
  }
  if (apiAvailable()) {
    Serial.println("Route lookup: APIs on flight detail only (AirLabs/FA/FR24)");
  } else if (apikeys::hasAirLabs() || apikeys::hasFlightAware() || apikeys::hasFr24()) {
    Serial.println("Route lookup: API keys saved but all providers disabled");
  } else {
    Serial.println("Route lookup: no API keys - prefix fallback only");
  }
}

void tickCacheFlush(unsigned long now_ms) {
  route_cache::tick(now_ms, readRamCacheSlot, kCacheSize, nowSec(),
                    config::kRouteLookupCacheTtlSec);
}

const char* sourceTag(ApiSource s) {
  switch (s) {
    case ApiSource::kCache:
      return "cache";
    case ApiSource::kAirLabs:
      return "AL";
    case ApiSource::kFlightAware:
      return "FA";
    case ApiSource::kFr24:
      return "FR";
    case ApiSource::kPrefix:
      return "pfx";
    default:
      return "";
  }
}

void enrichAircraft(services::adsb::Aircraft* planes, size_t count, double center_lat,
                    double center_lon) {
  (void)center_lat;
  (void)center_lon;

  for (size_t i = 0; i < count; ++i) {
    services::adsb::Aircraft& ac = planes[i];
    if (ac.callsign[0] == '\0' || !isIcaoRadioCallsign(ac.callsign)) {
      continue;
    }
    if (ac.airline[0] != '\0' && ac.route_origin[0] != '\0' && ac.route_dest[0] != '\0') {
      continue;
    }

    RouteInfo info;
    routeClear(&info);
    ApiSource src = ApiSource::kNone;
    if (cacheResolveRam(ac.callsign, &info, &src)) {
      applyRouteToAircraft(ac, info);
      continue;
    }

    if (ac.airline[0] == '\0' && lookupPrefixFallback(ac.callsign, &info)) {
      applyRouteToAircraft(ac, info);
      storeCache(ac.callsign, info, ApiSource::kPrefix, false);
    }
  }
}

void onFlightDetailSelected(const char* callsign, const bool immediate) {
  onFlightDetailSelectedImpl(callsign, immediate);
}

void tickDetailEnrichDebounce(unsigned long now_ms) {
  tickDetailEnrichDebounceImpl(now_ms);
}

void tickDetailWorkerWatchdog(unsigned long now_ms) {
  tickDetailWorkerWatchdogImpl(now_ms);
}

void cancelDetailEnrichment() { cancelDetailEnrichmentImpl(); }

bool detailEnrichmentReady() { return detailEnrichmentReadyImpl(); }

bool detailEnrichmentConsume() { return detailEnrichmentConsumeImpl(); }

bool detailWorkerBusy() { return s_detail_busy || s_detail_requested; }

}  // namespace services::route
