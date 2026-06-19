#include "ui/radar_display.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/aircraft_type_lookup.h"
#include "geo/flat_earth.h"
#include "services/map_center.h"
#include "ui/aircraft_symbol.h"
#include "ui/radar_accent.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorSweep = 0x0320;
uint16_t kColorSweepTrail = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitudeAscend = 0x07FF;
uint16_t kColorTagAltitudeDescend = 0xF81F;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
UiTextStyle s_cardinal_style = displayFontCardinal();
UiTextStyle s_scale_style = displayFontScale();
UiTextStyle s_tag_style = displayFontTag();

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

PlaneGfx* s_draw = &tft;
PlaneGfxSprite s_bg(&tft);
PlaneGfxSprite s_content(&tft);
bool s_bg_ready = false;
bool s_content_ready = false;

constexpr int kMaxSweepSpokes = 8;
float s_sweep_angle_deg = 0.0f;
bool s_sweep_track_valid = false;

struct CachedAircraftMarker {
  services::adsb::Aircraft plane{};
  int x = 0;
  int y = 0;
  bool beyond_dot = false;
};

CachedAircraftMarker s_prev_aircraft_markers[services::adsb::kMaxAircraft];
size_t s_prev_aircraft_marker_count = 0;

class DrawScope {
 public:
  explicit DrawScope(PlaneGfx& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  PlaneGfx* prev_;
};

UiTextStyle pickTextSizeForHeight(int target_px, size_t lo, size_t hi) {
  return displayFontPickForHeight(tft, target_px, lo, hi);
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  s_cardinal_style = pickTextSizeForHeight(radar::kCardinalLabelHeightPx, 2, 5);
  const int cardinal_h = displayFontHeight(tft, s_cardinal_style);
  s_scale_style =
      pickTextSizeForHeight(cardinal_h - radar::kScaleBelowCardinalPx, 0, 3);
  s_tag_style = pickTextSizeForHeight(radar::kAircraftTagLabelHeightPx, 2, 4);

  displayFontApply(tft, s_scale_style);
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kScaleBandCount; ++i) {
    for (int ring = 1; ring <= radar::kRingCount; ++ring) {
      const float ring_km = radar::kScaleBands[i].label_km *
                            static_cast<float>(ring) /
                            static_cast<float>(radar::kRingCount);
      for (bool miles : {false, true}) {
        radar::formatScaleTag(label, sizeof(label), ring_km, miles);
        const int w = tft.textWidth(label);
        if (w > s_scale_label_max_w) {
          s_scale_label_max_w = w;
        }
      }
    }
  }

  s_label_metrics_ready = true;
}

void initPalette() {
  const radar::AccentRgb accent = radar::accentPalette();
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(accent.grid_r, accent.grid_g, accent.grid_b);
  radar::kColorSweep = tft.color565(accent.sweep_r, accent.sweep_g, accent.sweep_b);
  radar::kColorSweepTrail =
      tft.color565(accent.trail_r, accent.trail_g, accent.trail_b);
  radar::kColorLabel = tft.color565(accent.label_r, accent.label_g, accent.label_b);
  radar::kColorAircraft =
      tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitudeAscend =
      tft.color565(radar::kTagAltAscendR, radar::kTagAltAscendG, radar::kTagAltAscendB);
  radar::kColorTagAltitudeDescend = tft.color565(radar::kTagAltDescendR,
                                                 radar::kTagAltDescendG,
                                                 radar::kTagAltDescendB);
}

/** Treat small negative rates as level (cyan). */
constexpr int16_t kDescendRateThresholdFpm = -64;

uint16_t altitudeTagColor(const services::adsb::Aircraft& plane) {
  if (plane.vert_rate_fpm != services::adsb::kVertRateUnknown &&
      plane.vert_rate_fpm < kDescendRateThresholdFpm) {
    return radar::kColorTagAltitudeDescend;
  }
  return radar::kColorTagAltitudeAscend;
}

void localOffsetFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                           float* dist_km) {
  geo::localOffsetKm(services::map_center::latitude(), services::map_center::longitude(),
                     lat, lon, dx_km, dy_km, dist_km);
}

float innerRingMaxKm() {
  const float outer_km = radar::scaleActive().label_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::scaleActive().label_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  localOffsetFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  localOffsetFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f || isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingMarker(int x, int y, float heading_deg) {
  aircraft_symbol::drawCompact(*s_draw, x, y, heading_deg, radar::kColorAircraft);
}

void applyTagStyle() { displayFontApply(*s_draw, s_tag_style); }

int aircraftSymbolHalfPx() {
  return std::max(aircraft_symbol::radiusPx(), radar::kAircraftIconRadiusPx);
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    max_w = std::max(max_w, s_draw->textWidth(plane.callsign));
  }
  if (plane.type[0] != '\0') {
    char type_label[32];
    services::aircraft_type::formatRadarTagLabel(plane.type, type_label, sizeof(type_label));
    max_w = std::max(max_w, s_draw->textWidth(type_label));
  }
  if (plane.alt[0] != '\0') {
    max_w = std::max(max_w, s_draw->textWidth(plane.alt));
  }
  return max_w;
}

void drawAircraftTag(int x, int y, const services::adsb::Aircraft& plane) {
  initLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half = aircraftSymbolHalfPx();
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(TextDatum::TopLeft);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(TextDatum::TopRight);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  ly += line_h;

  if (plane.type[0] != '\0') {
    char type_label[32];
    services::aircraft_type::formatRadarTagLabel(plane.type, type_label, sizeof(type_label));
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(type_label, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(altitudeTagColor(plane), radar::kColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    localOffsetFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x, &dot_y)) {
      continue;
    }
    dots[dot_count].index = i;
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    const size_t i = dots[d].index;
    drawBeyondRingMarker(dots[d].x, dots[d].y, planes[i].track_deg);
  }

  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    aircraft_symbol::draw(*s_draw, items[d].x, items[d].y, planes[i].track_deg,
                          radar::kColorAircraft);
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    drawAircraftTag(items[d].x, items[d].y, planes[i]);
  }
}

void drawCardinalLabel(const char* text, int x, int y, TextDatum datum) {
  displayFontApply(*s_draw, s_cardinal_style);
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  displayFontApply(*s_draw, s_scale_style);
  // Bottom-right anchor: text grows up/left into the W–SW wedge (not toward S).
  s_draw->setTextDatum(TextDatum::BottomRight);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2, radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void pointOnRadarArc(int cx, int cy, int r, float arc_len_px, int* x, int* y) {
  const float angle = arc_len_px / static_cast<float>(r);
  *x = cx + static_cast<int>(lroundf(sinf(angle) * static_cast<float>(r)));
  *y = cy - static_cast<int>(lroundf(cosf(angle) * static_cast<float>(r)));
}

void drawArcDash(int cx, int cy, int r, float arc_start_px, float arc_end_px, float half_width,
                 uint16_t color) {
  const float dash_len = arc_end_px - arc_start_px;
  if (dash_len < 0.5f) {
    return;
  }
  const int steps = std::max(2, static_cast<int>(lroundf(dash_len / 3.0f)));
  int px = 0;
  int py = 0;
  pointOnRadarArc(cx, cy, r, arc_start_px, &px, &py);
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float s = arc_start_px + dash_len * t;
    int nx = 0;
    int ny = 0;
    pointOnRadarArc(cx, cy, r, s, &nx, &ny);
    s_draw->drawWideLine(px, py, nx, ny, half_width, color);
    px = nx;
    py = ny;
  }
}

void drawDashedWideLine(int x0, int y0, int x1, int y1, float half_width, uint16_t color) {
  const float dx = static_cast<float>(x1 - x0);
  const float dy = static_cast<float>(y1 - y0);
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) {
    return;
  }

  const float ux = dx / len;
  const float uy = dy / len;
  const float dash = static_cast<float>(radar::kGridDashLenPx);
  const float gap = static_cast<float>(radar::kGridDashGapPx);
  const float period = dash + gap;

  for (float pos = 0.0f; pos + dash <= len; pos += period) {
    const float end = pos + dash;
    const int sx = x0 + static_cast<int>(lroundf(ux * pos));
    const int sy = y0 + static_cast<int>(lroundf(uy * pos));
    const int ex = x0 + static_cast<int>(lroundf(ux * end));
    const int ey = y0 + static_cast<int>(lroundf(uy * end));
    s_draw->drawWideLine(sx, sy, ex, ey, half_width, color);
  }
}

void drawDashedCircle(int cx, int cy, int r, float half_width, uint16_t color) {
  if (r <= 0) {
    return;
  }

  const float circumference = 2.0f * 3.14159265f * static_cast<float>(r);
  const float dash = static_cast<float>(radar::kGridDashLenPx);
  const float gap = static_cast<float>(radar::kGridDashGapPx);
  const float period = dash + gap;

  for (float arc_pos = 0.0f; arc_pos + dash <= circumference; arc_pos += period) {
    drawArcDash(cx, cy, r, arc_pos, arc_pos + dash, half_width, color);
  }
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const float hw = radar::kGridStrokeHalfWidth;
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    drawDashedCircle(cx, cy, r - i, hw, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawGridSpokes(int cx, int cy, int radius, uint16_t color) {
  const float hw = radar::kGridStrokeHalfWidth;
  drawDashedWideLine(cx, cy - radius, cx, cy + radius, hw, color);
  drawDashedWideLine(cx - radius, cy, cx + radius, cy, hw, color);

  constexpr float kInvSqrt2 = 0.70710678f;
  const int d = static_cast<int>(lroundf(static_cast<float>(radius) * kInvSqrt2));
  drawDashedWideLine(cx + d, cy - d, cx - d, cy + d, hw, color);
  drawDashedWideLine(cx - d, cy - d, cx + d, cy + d, hw, color);
}

void drawIntercardinalLabel(const char* text, int cx, int cy, int radius,
                            float angle_deg_from_north) {
  const float rad = angle_deg_from_north * 3.14159265f / 180.0f;
  const int r = radius - radar::kCardinalDiagonalInsetPx;
  const int x = cx + static_cast<int>(lroundf(sinf(rad) * static_cast<float>(r)));
  const int y = cy - static_cast<int>(lroundf(cosf(rad) * static_cast<float>(r)));
  drawCardinalLabel(text, x, y, TextDatum::MiddleCenter);
}

void drawCardinalLabels() {
  if (!radar::showCompassRose()) {
    return;
  }
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;
  const int rim_r = radar::kGridOuterRadius;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, TextDatum::TopCenter);
  drawCardinalLabel("S", cx, edge - radar::kCardinalSouthOffsetY, TextDatum::BottomCenter);
  drawCardinalLabel("W", 0, cy, TextDatum::MiddleLeft);
  drawCardinalLabel("E", edge, cy, TextDatum::MiddleRight);

  drawIntercardinalLabel("NE", cx, cy, rim_r, 45.0f);
  drawIntercardinalLabel("SE", cx, cy, rim_r, 135.0f);
  drawIntercardinalLabel("SW", cx, cy, rim_r, 225.0f);
  drawIntercardinalLabel("NW", cx, cy, rim_r, 315.0f);
}

/** Anchor scale text on the ring along kScaleLabelBearingDeg (between W and SW). */
void scaleLabelAnchorOnRing(int cx, int cy, int ring_radius, int gap_px, int* x,
                            int* y) {
  const float rad = radar::kScaleLabelBearingDeg * 3.14159265f / 180.0f;
  const float r = static_cast<float>(ring_radius - gap_px);
  *x = cx + static_cast<int>(lroundf(sinf(rad) * r));
  *y = cy - static_cast<int>(lroundf(cosf(rad) * r));
}

void drawRingScaleLabels(int cx, int cy, int outer_radius) {
  const float label_km = radar::scaleActive().label_km;
  const bool use_miles = radar::distanceInMiles();

  for (int ring = radar::kRingCount; ring >= 1; --ring) {
    const int ring_radius = (outer_radius * ring) / radar::kRingCount;
    const float ring_km =
        label_km * static_cast<float>(ring) / static_cast<float>(radar::kRingCount);

    char scale_label[12];
    radar::formatScaleTag(scale_label, sizeof(scale_label), ring_km, use_miles);

    int gap_px = radar::kScaleGapFromOuterRing;
    if (ring == radar::kRingCount && !use_miles) {
      gap_px = radar::kScaleGapOuterRingKm;
    }

    int ax = 0;
    int ay = 0;
    scaleLabelAnchorOnRing(cx, cy, ring_radius, gap_px, &ax, &ay);
    drawScaleLabelWithBackground(scale_label, ax, ay);
  }
}

template <typename GfxRef>
void drawStaticGrid(GfxRef& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  drawRings(cx, cy, grid_r);
  drawGridSpokes(cx, cy, grid_r, radar::kColorGrid);
  drawCardinalLabels();
  drawRingScaleLabels(cx, cy, grid_r);
  gfx.setTextDatum(TextDatum::TopLeft);
}

bool rebuildBackgroundSprite() {
  if (!s_bg_ready) {
    if (!s_bg.createSprite(radar::kSize, radar::kSize)) {
      Serial.println("radar: background sprite alloc failed");
      return false;
    }
    s_bg_ready = true;
  }

  drawStaticGrid(s_bg.gfx());
  s_sweep_track_valid = false;
  return true;
}

bool ensureContentSprite() {
  if (s_content_ready) {
    return true;
  }
  if (!s_content.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: content sprite alloc failed");
    return false;
  }
  s_content_ready = true;
  return true;
}

bool rebuildContentLayer() {
  if (!s_bg_ready || !ensureContentSprite()) {
    return false;
  }

  const int w = s_bg.width();
  const int h = s_bg.height();
  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  memcpy(s_content.bufferMut(), s_bg.buffer(), pixels * sizeof(uint16_t));

  {
    const DrawScope scope(s_content.gfx());
    drawAircraft();
  }

  return true;
}

void sweepSpokeEndpoint(float angle_deg, int* ex, int* ey) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = angle_deg * kDegToRad;
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kSweepRadiusPx;
  *ex = cx + static_cast<int>(lroundf(sinf(rad) * static_cast<float>(r)));
  *ey = cy - static_cast<int>(lroundf(cosf(rad) * static_cast<float>(r)));
}

struct IntRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  IntRect() = default;
  IntRect(int x_in, int y_in, int w_in, int h_in) : x(x_in), y(y_in), w(w_in), h(h_in) {}
};

IntRect s_prev_sweep_dirty;

bool rectEmpty(const IntRect& r) { return r.w <= 0 || r.h <= 0; }

IntRect rectFromPoints(int x0, int y0, int x1, int y1, int margin) {
  IntRect r;
  r.x = std::min(x0, x1) - margin;
  r.y = std::min(y0, y1) - margin;
  const int x2 = std::max(x0, x1) + margin;
  const int y2 = std::max(y0, y1) + margin;
  r.w = x2 - r.x + 1;
  r.h = y2 - r.y + 1;
  return r;
}

IntRect clampRectToScreen(IntRect r) {
  if (r.x < 0) {
    r.w += r.x;
    r.x = 0;
  }
  if (r.y < 0) {
    r.h += r.y;
    r.y = 0;
  }
  if (r.x + r.w > radar::kSize) {
    r.w = radar::kSize - r.x;
  }
  if (r.y + r.h > radar::kSize) {
    r.h = radar::kSize - r.y;
  }
  return r;
}

IntRect spokeBounds(float angle_deg, int margin) {
  int ex = 0;
  int ey = 0;
  sweepSpokeEndpoint(angle_deg, &ex, &ey);
  return clampRectToScreen(
      rectFromPoints(radar::kCenterX, radar::kCenterY, ex, ey, margin));
}

IntRect unionRect(const IntRect& a, const IntRect& b) {
  if (rectEmpty(a)) {
    return b;
  }
  if (rectEmpty(b)) {
    return a;
  }
  const int x0 = std::min(a.x, b.x);
  const int y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.x + a.w, b.x + b.w);
  const int y1 = std::max(a.y + a.h, b.y + b.h);
  return IntRect{x0, y0, x1 - x0, y1 - y0};
}

IntRect unionSpokeBounds(const float* angles, int count, int margin) {
  IntRect bounds{};
  for (int i = 0; i < count; ++i) {
    bounds = unionRect(bounds, spokeBounds(angles[i], margin));
  }
  return bounds;
}

void blitRegionFromContent(const IntRect& rect, const uint16_t* content, int stride) {
  if (rectEmpty(rect) || content == nullptr) {
    return;
  }
  IntRect clipped = clampRectToScreen(rect);
  if (rectEmpty(clipped)) {
    return;
  }

  const int area = clipped.w * clipped.h;
  constexpr int kScreenPixels = radar::kSize * radar::kSize;
  if (area >= kScreenPixels / 3 && s_content_ready) {
    s_content.pushSprite(0, 0);
    return;
  }

  tft.blitRegionFromBuffer(static_cast<int16_t>(clipped.x),
                           static_cast<int16_t>(clipped.y),
                           static_cast<int16_t>(clipped.w),
                           static_cast<int16_t>(clipped.h),
                           content + static_cast<size_t>(clipped.y) * static_cast<size_t>(stride) +
                               static_cast<size_t>(clipped.x),
                           static_cast<int16_t>(stride));
}

void drawSweepSpokeOn(PlaneGfx& gfx, float angle_deg, uint16_t color) {
  int ex = 0;
  int ey = 0;
  sweepSpokeEndpoint(angle_deg, &ex, &ey);
  gfx.drawWideLine(radar::kCenterX, radar::kCenterY, ex, ey, radar::kSweepLineHalfWidth,
                   color);
}

int collectSweepAngles(float lead_deg, float* angles, int max_angles) {
  if (max_angles <= 0) {
    return 0;
  }
  if (radar::kSweepTrailLines <= 1) {
    angles[0] = lead_deg;
    return 1;
  }

  int count = 0;
  for (int i = radar::kSweepTrailLines - 1; i >= 1; --i) {
    if (count >= max_angles - 1) {
      break;
    }
    const float t = static_cast<float>(i) / static_cast<float>(radar::kSweepTrailLines - 1);
    angles[count++] = lead_deg - t * radar::kSweepTrailSpanDeg;
  }
  if (count < max_angles) {
    angles[count++] = lead_deg;
  }
  return count;
}

float sweepStepDeg() {
  return 360.0f * static_cast<float>(radar::kSweepFrameMs) /
         static_cast<float>(radar::kSweepPeriodMs);
}

void advanceSweepAngle() {
  s_sweep_angle_deg += sweepStepDeg();
  if (s_sweep_angle_deg >= 360.0f) {
    s_sweep_angle_deg -= 360.0f;
  }
}

float currentSweepAngleDeg() { return s_sweep_angle_deg; }

IntRect aircraftMarkerBounds(int x, int y, const services::adsb::Aircraft& plane) {
  applyTagStyle();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = s_draw->fontHeight() * 3;
  const int symbol_half = aircraftSymbolHalfPx();
  constexpr int kPad = 6;

  int min_x = x - symbol_half - kPad;
  int max_x = x + symbol_half + kPad;
  int min_y = y - symbol_half - kPad;
  int max_y = y + symbol_half + kPad;

  if (x < radar::kCenterX) {
    max_x = std::max(max_x, x + symbol_half + radar::kAircraftLabelGapPx + kPad);
    min_x = std::min(min_x, x - symbol_half - block_w - radar::kAircraftLabelGapPx - kPad);
  } else {
    min_x = std::min(min_x, x - symbol_half - block_w - radar::kAircraftLabelGapPx - kPad);
    max_x = std::max(max_x, x + symbol_half + radar::kAircraftLabelGapPx + kPad);
  }
  min_y = std::min(min_y, y - block_h / 2 - kPad);
  max_y = std::max(max_y, y + block_h / 2 + kPad);

  return clampRectToScreen(
      IntRect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1));
}

size_t collectAircraftMarkers(CachedAircraftMarker* markers, size_t max_markers) {
  if (markers == nullptr || max_markers == 0) {
    return 0;
  }

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  size_t count = 0;

  for (size_t i = 0; i < n && count < max_markers; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    localOffsetFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    CachedAircraftMarker& marker = markers[count];
    marker.plane = planes[i];

    if (isInsideOuterRingKm(dist_km)) {
      latLonToScreen(planes[i].lat, planes[i].lon, &marker.x, &marker.y);
      marker.beyond_dot = false;
      ++count;
      continue;
    }

    if (beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &marker.x, &marker.y)) {
      marker.beyond_dot = true;
      ++count;
    }
  }
  return count;
}

IntRect markerBounds(const CachedAircraftMarker& marker) {
  if (marker.beyond_dot) {
    const int dot_r = aircraft_symbol::radiusPx() / 2 + 4;
    return clampRectToScreen(IntRect(marker.x - dot_r, marker.y - dot_r, dot_r * 2 + 1,
                                     dot_r * 2 + 1));
  }
  return aircraftMarkerBounds(marker.x, marker.y, marker.plane);
}

bool aircraftIdentityMatch(const services::adsb::Aircraft& a,
                           const services::adsb::Aircraft& b) {
  if (a.callsign[0] != '\0' && b.callsign[0] != '\0') {
    return strcmp(a.callsign, b.callsign) == 0;
  }
  constexpr float kPosEps = 0.001f;
  return fabsf(a.lat - b.lat) < kPosEps && fabsf(a.lon - b.lon) < kPosEps;
}

bool markerVisualChanged(const CachedAircraftMarker& prev,
                         const CachedAircraftMarker& curr) {
  if (prev.beyond_dot != curr.beyond_dot) {
    return true;
  }
  if (prev.x != curr.x || prev.y != curr.y) {
    return true;
  }
  if (fabsf(prev.plane.track_deg - curr.plane.track_deg) > 0.5f) {
    return true;
  }
  if (strcmp(prev.plane.callsign, curr.plane.callsign) != 0) {
    return true;
  }
  if (strcmp(prev.plane.type, curr.plane.type) != 0) {
    return true;
  }
  if (strcmp(prev.plane.alt, curr.plane.alt) != 0) {
    return true;
  }
  if (prev.plane.vert_rate_fpm != curr.plane.vert_rate_fpm) {
    return true;
  }
  return false;
}

IntRect unionChangedMarkerBounds(const CachedAircraftMarker* current, size_t curr_count) {
  bool prev_used[services::adsb::kMaxAircraft] = {};
  IntRect dirty{};

  for (size_t c = 0; c < curr_count; ++c) {
    int prev_i = -1;
    for (size_t p = 0; p < s_prev_aircraft_marker_count; ++p) {
      if (prev_used[p]) {
        continue;
      }
      if (aircraftIdentityMatch(s_prev_aircraft_markers[p].plane, current[c].plane)) {
        prev_i = static_cast<int>(p);
        break;
      }
    }

    if (prev_i >= 0) {
      prev_used[static_cast<size_t>(prev_i)] = true;
      if (markerVisualChanged(s_prev_aircraft_markers[static_cast<size_t>(prev_i)],
                              current[c])) {
        dirty = unionRect(dirty,
                          markerBounds(s_prev_aircraft_markers[static_cast<size_t>(prev_i)]));
        dirty = unionRect(dirty, markerBounds(current[c]));
      }
    } else {
      dirty = unionRect(dirty, markerBounds(current[c]));
    }
  }

  for (size_t p = 0; p < s_prev_aircraft_marker_count; ++p) {
    if (!prev_used[p]) {
      dirty = unionRect(dirty, markerBounds(s_prev_aircraft_markers[p]));
    }
  }

  return dirty;
}

void savePrevAircraftMarkers() {
  s_prev_aircraft_marker_count =
      collectAircraftMarkers(s_prev_aircraft_markers, services::adsb::kMaxAircraft);
}

void drawSweepSpoke(float angle_deg, uint16_t color) {
  drawSweepSpokeOn(tft, angle_deg, color);
}

void drawSweepAt(float lead_deg) {
  if (radar::kSweepTrailLines <= 1) {
    drawSweepSpoke(lead_deg, radar::kColorSweep);
    return;
  }

  for (int i = radar::kSweepTrailLines - 1; i >= 1; --i) {
    const float t = static_cast<float>(i) / static_cast<float>(radar::kSweepTrailLines - 1);
    const float angle = lead_deg - t * radar::kSweepTrailSpanDeg;
    drawSweepSpoke(angle, radar::kColorSweepTrail);
  }
  drawSweepSpoke(lead_deg, radar::kColorSweep);
}

}  // namespace

static void blitStatic() {
  initPalette();

  if (!s_bg_ready) {
    const DrawScope scope(tft);
    drawStaticGrid(tft);
    drawAircraft();
    tft.setTextDatum(TextDatum::TopLeft);
    s_sweep_track_valid = false;
    return;
  }

  if (!rebuildContentLayer()) {
    tft.startWrite();
    s_bg.pushSprite(0, 0);
    drawAircraft();
    tft.endWrite();
    tft.setTextDatum(TextDatum::TopLeft);
    s_sweep_track_valid = false;
    return;
  }

  tft.startWrite();
  s_content.pushSprite(0, 0);
  tft.endWrite();
  tft.setTextDatum(TextDatum::TopLeft);
  s_sweep_track_valid = false;
  savePrevAircraftMarkers();
}

void radarDisplayRefreshSweep() {
  initPalette();
  advanceSweepAngle();

  if (!s_content_ready) {
    const DrawScope scope(tft);
    drawSweepAt(currentSweepAngleDeg());
    drawAircraft();
    tft.setTextDatum(TextDatum::TopLeft);
    return;
  }

  const uint16_t* content = s_content.buffer();
  const int content_stride = s_content.width();

  float new_angles[kMaxSweepSpokes] = {};
  const int new_count =
      collectSweepAngles(currentSweepAngleDeg(), new_angles, kMaxSweepSpokes);

  constexpr int kSweepMargin =
      static_cast<int>(radar::kSweepLineHalfWidth * 2.0f + 4.0f);
  const IntRect new_dirty = unionSpokeBounds(new_angles, new_count, kSweepMargin);

  IntRect erase_dirty = new_dirty;
  if (s_sweep_track_valid) {
    erase_dirty = unionRect(s_prev_sweep_dirty, new_dirty);
  }
  if (!rectEmpty(erase_dirty)) {
    blitRegionFromContent(erase_dirty, content, content_stride);
  }

  for (int i = 0; i < new_count; ++i) {
    const uint16_t color =
        (i == new_count - 1) ? radar::kColorSweep : radar::kColorSweepTrail;
    drawSweepSpokeOn(tft, new_angles[i], color);
  }
  tft.setTextDatum(TextDatum::TopLeft);

  s_prev_sweep_dirty = new_dirty;
  s_sweep_track_valid = true;
}

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();
  s_sweep_angle_deg = 0.0f;
  s_sweep_track_valid = false;
  s_prev_aircraft_marker_count = 0;

  if (rebuildBackgroundSprite() && rebuildContentLayer()) {
    blitStatic();
    radarDisplayRefreshSweep();
    return;
  }

  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawSweepAt(currentSweepAngleDeg());
  drawAircraft();
  tft.setTextDatum(TextDatum::TopLeft);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  CachedAircraftMarker current[services::adsb::kMaxAircraft];
  const size_t curr_count =
      collectAircraftMarkers(current, services::adsb::kMaxAircraft);
  const IntRect dirty = unionChangedMarkerBounds(current, curr_count);
  if (rectEmpty(dirty)) {
    return;
  }

  if (!s_bg_ready || !rebuildContentLayer()) {
    blitStatic();
    radarDisplayRefreshSweep();
    return;
  }

  const uint16_t* content = s_content.buffer();
  const int content_stride = s_content.width();

  float hold_angles[kMaxSweepSpokes] = {};
  const int hold_count =
      collectSweepAngles(currentSweepAngleDeg(), hold_angles, kMaxSweepSpokes);

  blitRegionFromContent(dirty, content, content_stride);

  for (int i = 0; i < hold_count; ++i) {
    const uint16_t color =
        (i == hold_count - 1) ? radar::kColorSweep : radar::kColorSweepTrail;
    drawSweepSpokeOn(tft, hold_angles[i], color);
  }
  tft.setTextDatum(TextDatum::TopLeft);

  savePrevAircraftMarkers();
}

}  // namespace ui
