#include "services/route_lookup.h"

#include <Arduino.h>

#include "services/adsb_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cctype>
#include <cstring>
#include <ctime>

#include "config.h"
#include "services/airline_lookup.h"
#include "services/airport_lookup.h"
#include "services/api_keys.h"
#include "services/route_cache_store.h"

namespace services::route {

namespace {

constexpr char kAirLabsBase[] = "https://airlabs.co/api/v9/flight";
constexpr char kFlightAwareBase[] = "https://aeroapi.flightaware.com/aeroapi/flights/";
constexpr char kFr24Base[] =
    "https://fr24api.flightradar24.com/api/flight-summary/light";

constexpr size_t kCacheSize = 64;
/** New API lookups per ADS-B cycle (one unfamiliar callsign per cycle max). */
constexpr unsigned kMaxApiCallsPerFetch = 1;

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

bool apiAvailable();

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

bool fileEntryNeedsApiRouteUpgrade(const route_cache::Entry& e) {
  if (e.source == static_cast<uint8_t>(ApiSource::kPrefix)) {
    return true;
  }
  if (e.origin[0] != '\0' && e.dest[0] != '\0') {
    return false;
  }
  if (e.api_done && e.source == static_cast<uint8_t>(ApiSource::kNone)) {
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

/** First sight of a callsign: reserve a pending slot so live APIs run at most once. */
void ensureSeenCallsign(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return;
  }
  if (findCache(callsign) >= 0) {
    return;
  }
  route_cache::Entry file_entry;
  if (route_cache::lookupPermanent(callsign, &file_entry)) {
    return;
  }
  RouteInfo empty;
  routeClear(&empty);
  storeCache(callsign, empty, ApiSource::kNone, false);
}

bool apiLookupAlreadyDone(const char* callsign) {
  if (!apiAvailable()) {
    return true;
  }

  const int idx = findCache(callsign);
  if (idx >= 0) {
    return !slotNeedsApiRouteUpgrade(s_cache[idx]);
  }

  route_cache::Entry file_entry;
  if (!route_cache::lookupPermanent(callsign, &file_entry)) {
    return false;
  }
  return !fileEntryNeedsApiRouteUpgrade(file_entry);
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

bool httpGetJson(const char* url, const char* header_name, const char* header_value,
                 JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8);
  client.setHandshakeTimeout(8);
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  if (header_name != nullptr && header_value != nullptr) {
    http.addHeader(header_name, header_value);
  }
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);
  const int http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();
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
  if (!httpGetJson(url.c_str(), nullptr, nullptr, doc)) {
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

bool lookupAirLabs(const char* callsign, RouteInfo* route) {
  if (!apikeys::useAirLabs() || !apikeys::hasAirLabs() || route == nullptr) {
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
    if (ok) {
      return true;
    }
  }
  return false;
}

bool pickFlightAwareFlight(JsonArray flights, JsonObject* chosen) {
  for (JsonObject f : flights) {
    const char* status = f["status"].as<const char*>();
    if (status != nullptr && strcmp(status, "En Route") == 0 && !f["origin"].isNull() &&
        !f["destination"].isNull()) {
      *chosen = f;
      return true;
    }
  }
  for (JsonObject f : flights) {
    if (!f["origin"].isNull() && !f["destination"].isNull()) {
      *chosen = f;
      return true;
    }
  }
  if (!flights.isNull() && flights.size() > 0) {
    *chosen = flights[0].as<JsonObject>();
    return true;
  }
  return false;
}

bool lookupFlightAwareWithKey(const char* callsign, RouteInfo* route, const char* api_key) {
  if (api_key == nullptr || api_key[0] == '\0' || route == nullptr) {
    return false;
  }
  routeClear(route);

  String url = kFlightAwareBase;
  url += callsign;
  url += "?max_pages=1";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8);
  client.setHandshakeTimeout(8);
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.addHeader("x-apikey", api_key);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);
  const int http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    return false;
  }
  JsonArray flights = doc["flights"].as<JsonArray>();
  if (flights.isNull() || flights.size() == 0) {
    return false;
  }

  JsonObject flight;
  if (!pickFlightAwareFlight(flights, &flight)) {
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

bool lookupFlightAware(const char* callsign, RouteInfo* route) {
  if (!apikeys::useFlightAware() || !apikeys::hasFlightAware() || route == nullptr) {
    return false;
  }

  const size_t key_count = apikeys::flightAwareKeyCount();
  for (size_t i = 0; i < key_count; ++i) {
    if (!apikeys::canUseFlightAwareAt(i)) {
      continue;
    }
    const bool ok = lookupFlightAwareWithKey(callsign, route, apikeys::flightAwareKeyAt(i));
    apikeys::recordFlightAwareCallAt(i);
    if (ok) {
      return true;
    }
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

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Version", "v1");
  String auth = "Bearer ";
  auth += api_token;
  http.addHeader("Authorization", auth);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  http.setReuse(false);
  const int http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
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

bool lookupFr24(const char* callsign, RouteInfo* route) {
  if (!apikeys::useFr24() || !apikeys::hasFr24() || route == nullptr) {
    return false;
  }

  const size_t key_count = apikeys::fr24KeyCount();
  for (size_t i = 0; i < key_count; ++i) {
    if (!apikeys::canUseFr24At(i)) {
      continue;
    }
    const bool ok = lookupFr24WithKey(callsign, route, apikeys::fr24KeyAt(i));
    apikeys::recordFr24CallAt(i);
    if (ok) {
      return true;
    }
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

}  // namespace

void init() {
  apikeys::load();
  memset(s_cache, 0, sizeof(s_cache));
  if (route_cache::mount()) {
    Serial.println("Route lookup: flash cache mounted (/route_cache.csv)");
  }
  if (apiAvailable()) {
    Serial.println("Route lookup: APIs enabled (AirLabs/FA/FR24 waterfall)");
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

bool lookupFromApis(const char* callsign, RouteInfo* route, ApiSource* source_out) {
  if (route == nullptr) {
    return false;
  }
  routeClear(route);

  if (!apiAvailable() || apiLookupAlreadyDone(callsign)) {
    return false;
  }

  if (apikeys::useAirLabs() && apikeys::hasAirLabs() && apikeys::canUseAirLabs() &&
      lookupAirLabs(callsign, route)) {
    storeCache(callsign, *route, ApiSource::kAirLabs, true);
    if (source_out != nullptr) {
      *source_out = ApiSource::kAirLabs;
    }
    return true;
  }

  if (apikeys::useFlightAware() && apikeys::hasFlightAware() &&
      apikeys::canUseFlightAware() && lookupFlightAware(callsign, route)) {
    storeCache(callsign, *route, ApiSource::kFlightAware, true);
    if (source_out != nullptr) {
      *source_out = ApiSource::kFlightAware;
    }
    return true;
  }

  if (apikeys::useFr24() && apikeys::hasFr24() && apikeys::canUseFr24() &&
      lookupFr24(callsign, route)) {
    storeCache(callsign, *route, ApiSource::kFr24, true);
    if (source_out != nullptr) {
      *source_out = ApiSource::kFr24;
    }
    return true;
  }

  RouteInfo miss;
  routeClear(&miss);
  storeCache(callsign, miss, ApiSource::kNone, true);
  return false;
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

  if (count == 0) {
    return;
  }

  if (!apiAvailable()) {
    for (size_t i = 0; i < count; ++i) {
      if (planes[i].airline[0] != '\0') {
        continue;
      }
      RouteInfo info;
      ApiSource src = ApiSource::kNone;
      if (cacheResolve(planes[i].callsign, &info, &src)) {
        applyRouteToAircraft(planes[i], info);
        continue;
      }
      if (lookupPrefixFallback(planes[i].callsign, &info)) {
        applyRouteToAircraft(planes[i], info);
        storeCache(planes[i].callsign, info, ApiSource::kPrefix, false);
      }
    }
    return;
  }

  uint8_t order[services::adsb::kMaxAircraft];
  float dist_km[services::adsb::kMaxAircraft];
  for (size_t i = 0; i < count; ++i) {
    order[i] = static_cast<uint8_t>(i);
    const float cos_lat =
        cosf(static_cast<float>(center_lat * (3.14159265 / 180.0)));
    const float dx =
        static_cast<float>(planes[i].lon - center_lon) * 111.0f * cos_lat;
    const float dy = static_cast<float>(planes[i].lat - center_lat) * 111.0f;
    dist_km[i] = sqrtf(dx * dx + dy * dy);
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

  unsigned api_calls = 0;

  for (size_t row = 0; row < count; ++row) {
    services::adsb::Aircraft& ac = planes[order[row]];
    if (ac.callsign[0] == '\0' || !isIcaoRadioCallsign(ac.callsign)) {
      continue;
    }

    RouteInfo info;
    routeClear(&info);
    ApiSource src = ApiSource::kNone;
    const bool route_complete = cacheResolve(ac.callsign, &info, &src);
    if (routeHasData(info)) {
      applyRouteToAircraft(ac, info);
    }
    if (route_complete) {
      continue;
    }

    ensureSeenCallsign(ac.callsign);

    if (api_calls >= kMaxApiCallsPerFetch) {
      continue;
    }

    if (apiLookupAlreadyDone(ac.callsign)) {
      continue;
    }

    if (lookupFromApis(ac.callsign, &info, &src)) {
      ++api_calls;
      applyRouteToAircraft(ac, info);
      logRouteLine(ac.callsign, info, sourceTag(src));
      continue;
    }

    if (lookupPrefixFallback(ac.callsign, &info)) {
      applyRouteToAircraft(ac, info);
      storeCache(ac.callsign, info, ApiSource::kPrefix, true);
      logRouteLine(ac.callsign, info, "pfx");
    }
  }
}

}  // namespace services::route
