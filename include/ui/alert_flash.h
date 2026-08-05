#pragma once

#include <cstdint>

namespace ui {

enum class AlertFlashKind : uint8_t {
  Tracked,
  Military,
};

/** Start a 10s blinking ring: red for military, green for other tracked aircraft. */
void alertFlashStart(AlertFlashKind kind);

/** True while the red ring flash window is active. */
bool alertFlashActive();

/**
 * Paint the alert ring into an RGB565 framebuffer (radar content sprite).
 * Call from the radar rebuild path so the ring rides along with normal blits —
 * no per-frame SPI cost that would stall the sweep.
 */
void alertFlashPaintBuffer(uint16_t* buf, int16_t w, int16_t h);

/**
 * Call once per loop. Does not draw. Returns true when the flash ends (caller
 * should redraw the UI to clear the ring).
 */
bool alertFlashPoll();

}  // namespace ui
