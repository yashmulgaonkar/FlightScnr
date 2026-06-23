#pragma once

#include <cstddef>

namespace services::airport {

/** Resolve airport code (ICAO or IATA) to city label from flash table (mwgg/Airports). */
bool lookupName(const char* code, char* out, size_t out_len);

/** Normalize to 4-letter ICAO when possible (ICAO passthrough or IATA remap). */
bool normalizeRouteCode(const char* code, char* icao_out, size_t icao_len);

}  // namespace services::airport
