#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {
struct Aircraft;
}

namespace services::route {

/** Which API supplied the airline name (serial log tag). */
enum class ApiSource : uint8_t {
  kNone = 0,
  kCache,
  kAirLabs,
  kFlightAware,
  kFr24,
  kPrefix,  // callsign-prefix fallback when no API keys / all failed
};

void init();

/** Debounced persist of route cache to LittleFS (call from main loop). */
void tickCacheFlush(unsigned long now_ms);

/** Apply RAM/flash cache and prefix fallback during ADS-B polls (no live APIs). */
void enrichAircraft(services::adsb::Aircraft* planes, size_t count, double center_lat,
                    double center_lon);

/**
 * Flight-detail view opened or encoder moved to another aircraft.
 * immediate=true on open/tap; false on encoder steps (debounced before API).
 */
void onFlightDetailSelected(const char* callsign, bool immediate = false);

/** Fire debounced detail enrich after encoder settles (call from main loop). */
void tickDetailEnrichDebounce(unsigned long now_ms);

/** Log and queue pending when route worker is slow on a stale callsign (call from main loop). */
void tickDetailWorkerWatchdog(unsigned long now_ms);

/** Clear detail enrichment state when leaving flight detail. */
void cancelDetailEnrichment();

/** True when a background detail enrichment finished. */
bool detailEnrichmentReady();

/** Apply enrichment result to the aircraft list; returns true if UI should refresh. */
bool detailEnrichmentConsume();

/** True when route detail worker is running or queued. */
bool detailWorkerBusy();

const char* sourceTag(ApiSource s);

}  // namespace services::route
