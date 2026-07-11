#include "ui/aircraft_symbol.h"

#include <cmath>

#include "hardware/plane_gfx.h"
#include "services/adsb_client.h"
#include "ui/aircraft_icon.h"

namespace ui::aircraft_symbol {
namespace {

constexpr float kDegToRad = 0.01745329252f;
constexpr int kFullSidePx = 28;
constexpr int kCompactSidePx = 16;

/** Local +y = aft, -y = nose. Same basis as radar sweep / noseTip. */
void mapLocal(int lx, int ly, int cx, int cy, float heading_deg, int* ox, int* oy) {
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);
  *ox = cx + static_cast<int>(lroundf(static_cast<float>(lx) * cos_h -
                                        static_cast<float>(ly) * sin_h));
  *oy = cy + static_cast<int>(lroundf(static_cast<float>(lx) * sin_h +
                                        static_cast<float>(ly) * cos_h));
}

void fillTri(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color, int x0,
             int y0, int x1, int y1, int x2, int y2) {
  int sx0 = 0;
  int sy0 = 0;
  int sx1 = 0;
  int sy1 = 0;
  int sx2 = 0;
  int sy2 = 0;
  mapLocal(x0, y0, cx, cy, heading_deg, &sx0, &sy0);
  mapLocal(x1, y1, cx, cy, heading_deg, &sx1, &sy1);
  mapLocal(x2, y2, cx, cy, heading_deg, &sx2, &sy2);
  gfx.fillTriangle(sx0, sy0, sx1, sy1, sx2, sy2, color);
}

void lineLocal(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
               float half_width, int x0, int y0, int x1, int y1) {
  int sx0 = 0;
  int sy0 = 0;
  int sx1 = 0;
  int sy1 = 0;
  mapLocal(x0, y0, cx, cy, heading_deg, &sx0, &sy0);
  mapLocal(x1, y1, cx, cy, heading_deg, &sx1, &sy1);
  gfx.drawWideLine(sx0, sy0, sx1, sy1, half_width, color);
}

struct IconScale {
  int8_t nose_y;
  int8_t tail_y;
  int8_t wing_x;
  int8_t wing_y;
  int8_t tail_fin_y;
};

/** Option B: open “V” wings + fuselage stem + small tail (reads clearly at 12 px). */
constexpr IconScale kFull = {11, 5, 7, -1, 7};
constexpr IconScale kCompact = {6, 3, 4, 0, 4};

void drawPlaneIcon(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
                   const IconScale& s) {
  const int ny = -s.nose_y;
  const int wy = s.wing_y;
  const int wx = s.wing_x;
  const int ty = s.tail_y;
  const int fin = s.tail_fin_y;

  lineLocal(gfx, cx, cy, heading_deg, color, 1.4f, 0, ny, 0, ty);
  lineLocal(gfx, cx, cy, heading_deg, color, 1.3f, -wx, wy, 0, ny);
  lineLocal(gfx, cx, cy, heading_deg, color, 1.3f, wx, wy, 0, ny);
  fillTri(gfx, cx, cy, heading_deg, color, 0, ny, -2, ny + 2, 2, ny + 2);
  lineLocal(gfx, cx, cy, heading_deg, color, 1.0f, -2, fin, 2, fin);
  fillTri(gfx, cx, cy, heading_deg, color, 0, fin, -2, ty, 2, ty);
}

void drawWithOptionalIcon(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
                          const services::adsb::Aircraft* aircraft, int base_side,
                          const IconScale& fallback) {
  if (aircraft != nullptr) {
    const uint8_t cat = aircraft_icon::resolveCategory(*aircraft);
    if (aircraft_icon::draw(gfx, cx, cy, heading_deg, color, cat, base_side)) {
      return;
    }
  }
  drawPlaneIcon(gfx, cx, cy, heading_deg, color, fallback);
}

}  // namespace

int radiusPx() { return 15; }

void draw(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
          const services::adsb::Aircraft* aircraft) {
  drawWithOptionalIcon(gfx, cx, cy, heading_deg, color, aircraft, kFullSidePx, kFull);
}

void drawCompact(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
                 const services::adsb::Aircraft* aircraft) {
  drawWithOptionalIcon(gfx, cx, cy, heading_deg, color, aircraft, kCompactSidePx,
                       kCompact);
}

}  // namespace ui::aircraft_symbol
