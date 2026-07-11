#pragma once

#include <cstdint>

class PlaneGfx;

namespace services::adsb {
struct Aircraft;
}

namespace ui::aircraft_symbol {

/** Top-down aircraft glyph radius in pixels (for layout / dirty rects). */
int radiusPx();

/** Full-size symbol on the radar grid, rotated so nose points along heading_deg.
 *  Uses Pi-style category PNG silhouette when available; else vector V-wing. */
void draw(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
          const services::adsb::Aircraft* aircraft = nullptr);

/** Smaller symbol for beyond-range rim markers. */
void drawCompact(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
                 const services::adsb::Aircraft* aircraft = nullptr);

}  // namespace ui::aircraft_symbol
