#include "services/adsbdb_parse.h"

#include <cctype>
#include <cstring>

#include "services/route_info.h"

namespace services::route {

namespace {

using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

void routeClear(RouteInfo* r) {
  r->airline[0] = '\0';
  r->airline_icao[0] = '\0';
  r->origin[0] = '\0';
  r->dest[0] = '\0';
}

bool routeHasData(const RouteInfo& r) {
  return r.airline[0] != '\0' || r.airline_icao[0] != '\0' || r.origin[0] != '\0' ||
         r.dest[0] != '\0';
}

// Copy an ICAO airport code: keep alnum only, uppercase, cap at out_len-1
// (max 4 chars for the RouteInfo buffers). Leaves out empty on no input.
void copyAirportCode(const char* s, char* out, size_t out_len) {
  out[0] = '\0';
  if (s == nullptr || out_len == 0) {
    return;
  }
  size_t n = 0;
  for (size_t i = 0; s[i] != '\0' && n + 1 < out_len; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (!std::isalnum(c)) {
      continue;
    }
    out[n++] = static_cast<char>(std::toupper(c));
  }
  out[n] = '\0';
}

// Copy an airline ICAO code: only accept exactly 3 alpha chars, uppercased.
void copyAirlineIcao(const char* s, char* out, size_t out_len) {
  out[0] = '\0';
  if (s == nullptr || out_len < 4 || std::strlen(s) != 3) {
    return;
  }
  for (int i = 0; i < 3; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (!std::isalpha(c)) {
      out[0] = '\0';
      return;
    }
    out[i] = static_cast<char>(std::toupper(c));
  }
  out[3] = '\0';
}

void copyTruncated(const char* s, char* out, size_t out_len) {
  out[0] = '\0';
  if (s == nullptr || out_len == 0) {
    return;
  }
  std::strncpy(out, s, out_len - 1);
  out[out_len - 1] = '\0';
}

}  // namespace

bool parseAdsbdbResponse(JsonDocument& doc, RouteInfo* route) {
  if (route == nullptr) {
    return false;
  }
  routeClear(route);

  JsonObjectConst flightroute = doc["response"]["flightroute"].as<JsonObjectConst>();
  if (flightroute.isNull()) {
    return false;
  }

  copyAirportCode(flightroute["origin"]["icao_code"].as<const char*>(), route->origin,
                  sizeof(route->origin));
  copyAirportCode(flightroute["destination"]["icao_code"].as<const char*>(), route->dest,
                  sizeof(route->dest));

  JsonObjectConst airline = flightroute["airline"].as<JsonObjectConst>();
  if (!airline.isNull()) {
    copyAirlineIcao(airline["icao"].as<const char*>(), route->airline_icao,
                    sizeof(route->airline_icao));
    copyTruncated(airline["name"].as<const char*>(), route->airline, sizeof(route->airline));
  }

  return routeHasData(*route);
}

void buildAdsbdbFilter(JsonDocument& filter) {
  auto flightroute = filter["response"]["flightroute"];
  flightroute["airline"]["name"] = true;
  flightroute["airline"]["icao"] = true;
  flightroute["origin"]["icao_code"] = true;
  flightroute["destination"]["icao_code"] = true;
}

}  // namespace services::route
