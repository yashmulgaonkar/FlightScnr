#pragma once

#include <cstddef>
#include <cstdint>

namespace ui {

/**
 * Round flight-detail view (layout uses config display size; safe on circular panels).
 * Knob/tap on radar opens closest or tapped aircraft; encoder cycles targets on this screen.
 */
void flightDetailDraw();

/** Update alt/speed in place when ADS-B data changes; full redraw if layout changes. */
void flightDetailRefresh();

/** Redraw when no-API alternate labels toggle (call from main loop on flight detail). */
void flightDetailTick(unsigned long now_ms);

/** Rebuild list sorted by distance; select closest aircraft. */
void flightDetailSelectClosest();

/** Select aircraft nearest screen tap (radar coordinates). */
void flightDetailSelectAtScreen(int16_t x, int16_t y);

/** Cycle selection (+1 / -1). Returns false if no aircraft. */
bool flightDetailCycle(int delta);

/** Callsign of the aircraft currently selected on this screen (nullptr if none). */
const char* flightDetailSelectedCallsign();

/** Mode-S hex of the selected aircraft (nullptr if unknown). */
const char* flightDetailSelectedHex();

/** Free the off-screen canvas (~300KB PSRAM) when leaving flight detail. */
void flightDetailReleaseSprite();

/** True when the detail draw buffer is allocated. */
bool flightDetailSpriteReady();

/** Mark that enrich just redrew this callsign (coalesce duplicate full draws). */
void flightDetailMarkEnrichRedrawn(const char* callsign);

/** True if callsign was fully redrawn within window_ms (0 = any callsign). */
bool flightDetailRecentlyRedrawn(const char* callsign, unsigned long window_ms);

/** True if callsign was drawn with route/airline shown (not "Fetching…") within window_ms. */
bool flightDetailRecentlyShowedRoute(const char* callsign, unsigned long window_ms);

/** Show/hide the 30s enrich failsafe banner on the detail screen. */
void flightDetailSetEnrichFailsafe(bool shown);

/** True when the enrich failsafe banner should be drawn. */
bool flightDetailEnrichFailsafe();

/**
 * Idle auto-return countdown ring on the screen rim.
 * active=false or timeout_ms==0 hides the ring (Manual / waiting to settle).
 * Ring is full at started_ms and empties CCW from top (erases leftward).
 */
void flightDetailSetIdleCountdown(bool active, unsigned long started_ms,
                                  unsigned long timeout_ms);

}  // namespace ui
