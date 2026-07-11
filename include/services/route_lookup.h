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

/**
 * Tear down the route_detail FreeRTOS task (16KB internal stack) when idle.
 * Call when leaving flight detail or during heap recovery while not on detail —
 * next enrich recreates the worker via ensureDetailWorker().
 */
void shutdownDetailWorker();

/** Route worker step name for resume diagnostics. */
const char* detailWorkerDebugStepTag();

/** True when route worker is waiting for the detail sprite to be released. */
bool detailWorkerDebugSpriteReleasePending();

/** True when a background detail enrichment finished. */
bool detailEnrichmentReady();

/** Apply enrichment result to the aircraft list; returns true if consumed.
 *  When needs_redraw is non-null, set false to skip a redundant full redraw. */
bool detailEnrichmentConsume(bool* needs_redraw = nullptr);

/** True when route detail worker is running or queued. */
bool detailWorkerBusy();

/** True when ADS-B HTTPS should not start (detail enrich/debounce pending). */
bool detailAdsbFetchPaused();

/**
 * True only while the detail sprite is mid-release for TLS heap (the loop tears
 * it down then, so the buffer must not be drawn into). A merely-busy route
 * worker does NOT block drawing: the sprite is PSRAM-backed and only created or
 * freed on the loop task, so scroll redraws run immediately while route
 * enrichment continues in the background and fills in the route line when ready.
 */
bool detailDrawUnsafe();

/** Release flight-detail sprite when the route worker requests it (main loop only). */
void tickDetailSpriteRelease();

/** Current flight-detail selection tracked by the route worker (may be empty). */
const char* detailSelectionCallsign();

/** True when at least one live route API can be used (AirLabs / FA / FR24). */
bool liveRouteApiAvailable();

/** True when live route enrichment is pending or running for this callsign. */
bool detailEnrichmentInFlight(const char* callsign);

/** Route worker saw a transient TLS/memory failure — request faster WiFi recycle. */
void noteTlsMemoryFailure();

/** True if a TLS recover request is pending (does not consume it). */
bool tlsRecoverRequested();

/** True once after noteTlsMemoryFailure (cleared on read). */
bool consumeTlsRecoverRequest();

/** Clear latched route TLS failure state (e.g. after WiFi recycle). */
void resetTlsHardFail();

const char* sourceTag(ApiSource s);

}  // namespace services::route
