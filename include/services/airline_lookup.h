#pragma once

#include <cstddef>

namespace services::airline {

/** Look up display name by 2- or 3-letter airline code (ICAO or IATA). */
bool lookupByCode(const char* code, char* out, size_t out_len);

/**
 * Resolve airline from ADS-B flight field (plane-tracker / routelookup rules).
 * Only when has_flight_field is true (not mode-s hex). ICAO: AAL1234; IATA: UA1234.
 */
void resolveFromCallsign(const char* callsign, bool has_flight_field, char* out,
                         size_t out_len);

/** Extract 3-letter ICAO operator code from callsign when possible. */
bool resolveIcaoFromCallsign(const char* callsign, bool has_flight_field, char* out,
                             size_t out_len);

/** Build IATA flight code (e.g. BAW5WB -> BA5WB) for AirLabs flight_iata queries. */
bool buildFlightIataFromCallsign(const char* callsign, char* out, size_t out_len);

/**
 * Strip operational suffix letters after the flight number (BAW71LK -> BAW71).
 * Returns false when no shorter variant applies.
 */
bool buildCallsignApiVariant(const char* callsign, char* out, size_t out_len);

}  // namespace services::airline
