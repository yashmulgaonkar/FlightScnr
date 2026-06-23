#pragma once

#include <cstdint>

class PlaneGfx;

namespace services::airline {

/** Draw airline logo for ICAO operator code (3 letters); returns true if drawn. */
bool drawLogo(PlaneGfx& tft, const char* icao, int16_t center_x, int16_t y, uint16_t bg);

/** Logo height in pixels for layout (0 when unknown / missing). */
int logoHeightForIcao(const char* icao);

/** Free decode buffer (~15KB) when leaving flight detail. */
void releaseLogoBuffer();

}  // namespace services::airline
