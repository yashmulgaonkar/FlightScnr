#pragma once

#include <ArduinoJson.h>

#include "services/route_info.h"

namespace services::route {

/**
 * Parse an adsbdb `GET /v0/callsign/{callsign}` response into a RouteInfo.
 *
 * Pure, hardware-free transformation (test seam). Reads
 * `response.flightroute.{airline.{name,icao},origin.icao_code,
 * destination.icao_code}`. Clears `route` first, then fills:
 *   - origin/dest: uppercase, alnum only, truncated to 4 chars.
 *   - airline_icao: exactly 3 alpha chars, uppercased; otherwise left empty.
 *   - airline: name copied and truncated to the RouteInfo buffer.
 *
 * Returns true when any usable field (origin, dest, airline or airline_icao)
 * was populated. A non-object `flightroute` (e.g. `"response":"unknown
 * callsign"`) yields an empty route and false.
 */
bool parseAdsbdbResponse(ArduinoJson::JsonDocument& doc, RouteInfo* route);

/**
 * Configure a deserialization filter limiting the parsed document to the
 * fields consumed by parseAdsbdbResponse (heap guard, analogous to the other
 * route filters).
 */
void buildAdsbdbFilter(ArduinoJson::JsonDocument& filter);

}  // namespace services::route
