#pragma once

#include <cstdint>

class PlaneGfx;

namespace services::photo {

void init();

/**
 * Flight-detail selection changed.
 * immediate=true on open/tap; false on encoder steps (debounced).
 * hex may be empty — then no fetch is queued.
 */
void onFlightDetailSelected(const char* callsign, const char* hex, bool immediate = false);

/** Fire debounced photo fetch after encoder settles (main loop). */
void tickDebounce(unsigned long now_ms);

/** Clear selection / drop in-flight result when leaving detail or scrolling away. */
void cancel();

/** True when a photo job finished for the current selection. */
bool ready();

/** Consume ready result; set needs_redraw when the UI should refresh. */
bool consume(bool* needs_redraw = nullptr);

/** True while a photo fetch is pending/running for this callsign. */
bool inFlight(const char* callsign);

/**
 * True when the photo attempt for this selection has finished hard:
 * no hex / nothing to fetch, image shown, known no-photo, or hard failure.
 * Soft retry / debounce / job running → false.
 */
bool settled(const char* callsign);

/** Decoded image height for layout (0 if none for callsign). */
int imageHeight(const char* callsign);

/** Photographer credit for callsign (empty if none). */
const char* photographer(const char* callsign);

/** Draw decoded RGB565 photo centered at center_x / top y. */
bool draw(PlaneGfx& gfx, const char* callsign, int16_t center_x, int16_t y);

/** Free JPEG/frame buffers (leave detail / HTTPS pressure). */
void releaseBuffers();

}  // namespace services::photo
