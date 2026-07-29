#include "services/route_waterfall.h"

#include <cstring>

namespace services::route {

bool routeEndpointsComplete(const RouteInfo& r) {
  return r.origin[0] != '\0' && r.dest[0] != '\0';
}

bool shouldFinishLiveApiStep(const RouteInfo& r) { return routeEndpointsComplete(r); }

void mergePartialRoute(RouteInfo* dest, const RouteInfo& partial) {
  if (dest == nullptr) {
    return;
  }
  if (dest->airline[0] == '\0' && partial.airline[0] != '\0') {
    std::strncpy(dest->airline, partial.airline, sizeof(dest->airline) - 1);
    dest->airline[sizeof(dest->airline) - 1] = '\0';
  }
  if (dest->airline_icao[0] == '\0' && partial.airline_icao[0] != '\0') {
    std::strncpy(dest->airline_icao, partial.airline_icao, sizeof(dest->airline_icao) - 1);
    dest->airline_icao[sizeof(dest->airline_icao) - 1] = '\0';
  }
  if (dest->origin[0] == '\0' && partial.origin[0] != '\0') {
    std::strncpy(dest->origin, partial.origin, sizeof(dest->origin) - 1);
    dest->origin[sizeof(dest->origin) - 1] = '\0';
  }
  if (dest->dest[0] == '\0' && partial.dest[0] != '\0') {
    std::strncpy(dest->dest, partial.dest, sizeof(dest->dest) - 1);
    dest->dest[sizeof(dest->dest) - 1] = '\0';
  }
}

}  // namespace services::route
