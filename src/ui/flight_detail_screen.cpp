#include "ui/flight_detail_screen.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/aircraft_type_lookup.h"
#include "services/airport_lookup.h"
#include "data/icao_types_lookup.h"
#include "data/airports_lookup.h"
#include "geo/flat_earth.h"
#include "services/map_center.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace ui {
namespace {

constexpr int kBezelInsetPx = 10;
constexpr int kTextPadPx = 6;
constexpr int kTitleGap = 4;
constexpr int kLineGap = 3;
constexpr int kSectionGap = 6;
constexpr int kFooterGap = 6;
constexpr int kTapPickRadiusPx = 36;
constexpr size_t kRouteLabelLen = data::airports::kMaxNameLen + 6;  // "ICAO, " + name

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

uint8_t s_order[services::adsb::kMaxAircraft];
size_t s_order_count = 0;
size_t s_sel = 0;

float aircraftDistKm(const services::adsb::Aircraft& ac) {
  float dx = 0.0f;
  float dy = 0.0f;
  float dist = 0.0f;
  geo::localOffsetKm(services::map_center::latitude(), services::map_center::longitude(),
                     ac.lat, ac.lon, &dx, &dy, &dist);
  return dist;
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::scaleActive().label_km;
  const float px_per_km =
      static_cast<float>(radar::kGridOuterRadius) / outer_km;
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  geo::localOffsetKm(services::map_center::latitude(), services::map_center::longitude(),
                     lat, lon, &dx_km, &dy_km, &dist_km);
  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

int circleHalfWidthAtRow(int row_y, int row_h) {
  if (kCircleRadius <= 0 || row_h <= 0) {
    return 0;
  }
  const int row_center_y = row_y + row_h / 2;
  const int dy = row_center_y - kCenterY;
  if (std::abs(dy) >= kCircleRadius) {
    return 0;
  }
  const float half =
      std::sqrt(static_cast<float>(kCircleRadius * kCircleRadius - dy * dy));
  const int usable = static_cast<int>(half) - kTextPadPx;
  return usable > 0 ? usable : 0;
}

void fitLineToWidth(char* text, size_t len, UiTextStyle style, int max_width_px) {
  if (max_width_px <= 0 || text[0] == '\0') {
    return;
  }
  displayFontApply(tft, style);
  if (tft.textWidth(text) <= max_width_px) {
    return;
  }
  char original[96];
  strncpy(original, text, sizeof(original) - 1);
  original[sizeof(original) - 1] = '\0';
  const size_t raw_len = strlen(original);
  for (size_t n = raw_len; n > 0; --n) {
    snprintf(text, len, "%.*s…", static_cast<int>(n), original);
    if (tft.textWidth(text) <= max_width_px) {
      return;
    }
  }
  strncpy(text, "…", len);
  text[len - 1] = '\0';
}

bool nextWrappedLine(const char** cursor, char* line, size_t line_len, UiTextStyle style,
                     int max_width_px, bool last_line) {
  if (cursor == nullptr || line_len == 0) {
    return false;
  }
  const char* p = *cursor;
  while (*p == ' ') {
    ++p;
  }
  if (*p == '\0') {
    return false;
  }

  line[0] = '\0';
  displayFontApply(tft, style);

  if (last_line) {
    strncpy(line, p, line_len - 1);
    line[line_len - 1] = '\0';
    fitLineToWidth(line, line_len, style, max_width_px);
    *cursor = p + strlen(p);
    return line[0] != '\0';
  }

  while (*p != '\0') {
    const char* word_start = p;
    while (*p != '\0' && *p != ' ') {
      ++p;
    }
    const size_t word_len = static_cast<size_t>(p - word_start);

    char trial[64];
    if (line[0] == '\0') {
      snprintf(trial, sizeof(trial), "%.*s", static_cast<int>(word_len), word_start);
    } else {
      snprintf(trial, sizeof(trial), "%s %.*s", line,
               static_cast<int>(word_len), word_start);
    }

    if (tft.textWidth(trial) <= max_width_px) {
      strncpy(line, trial, line_len - 1);
      line[line_len - 1] = '\0';
      while (*p == ' ') {
        ++p;
      }
      continue;
    }

    if (line[0] != '\0') {
      *cursor = word_start;
      return true;
    }

    for (size_t n = word_len; n > 0; --n) {
      snprintf(trial, sizeof(trial), "%.*s", static_cast<int>(n), word_start);
      if (tft.textWidth(trial) <= max_width_px) {
        strncpy(line, trial, line_len - 1);
        line[line_len - 1] = '\0';
        *cursor = word_start + n;
        return true;
      }
    }

    line[0] = word_start[0];
    line[1] = '\0';
    *cursor = word_start + 1;
    return true;
  }

  *cursor = p;
  return line[0] != '\0';
}

int wrappedLineCount(const char* text, UiTextStyle style, int start_y, int line_h,
                     int max_lines) {
  if (text == nullptr || text[0] == '\0') {
    return 1;
  }

  const char* cursor = text;
  char line[64];
  int count = 0;
  int y = start_y;

  while (count < max_lines) {
    const int max_w = circleHalfWidthAtRow(y, line_h) * 2;
    if (max_w <= 0) {
      break;
    }
    const bool last_line = count + 1 >= max_lines;
    if (!nextWrappedLine(&cursor, line, sizeof(line), style, max_w, last_line)) {
      break;
    }
    ++count;
    y += line_h + kLineGap;
    if (*cursor == '\0') {
      break;
    }
  }

  return count > 0 ? count : 1;
}

void drawCenterWrapped(const char* text, int* y, UiTextStyle style, uint16_t fg, uint16_t bg,
                       int max_lines) {
  displayFontApply(tft, style);
  const int line_h = displayFontHeight(tft, style);
  const char* cursor = text;
  char line[64];

  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);

  for (int i = 0; i < max_lines; ++i) {
    const int max_w = circleHalfWidthAtRow(*y, line_h) * 2;
    if (max_w <= 0) {
      break;
    }
    const bool last_line = i + 1 >= max_lines;
    if (!nextWrappedLine(&cursor, line, sizeof(line), style, max_w, last_line)) {
      break;
    }
    tft.drawString(line, kCenterX, *y);
    *y += line_h + kLineGap;
    if (*cursor == '\0') {
      break;
    }
  }
}

void drawCenterLine(const char* text, int* y, UiTextStyle style, uint16_t fg,
                    uint16_t bg) {
  displayFontApply(tft, style);
  const int h = displayFontHeight(tft, style);
  const int max_half_w = circleHalfWidthAtRow(*y, h);
  const int max_w = max_half_w * 2;

  char line[48];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  fitLineToWidth(line, sizeof(line), style, max_w);

  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(line, kCenterX, *y);
  *y += h + kLineGap;
}

void redrawCenterLineAt(int y, const char* text, UiTextStyle style, uint16_t fg,
                        uint16_t bg) {
  displayFontApply(tft, style);
  const int h = displayFontHeight(tft, style);
  const int max_half_w = circleHalfWidthAtRow(y, h);
  const int max_w = max_half_w * 2;

  char line[48];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  fitLineToWidth(line, sizeof(line), style, max_w);

  if (max_w > 0) {
    tft.fillRect(kCenterX - max_half_w, y, max_w, h, bg);
  }
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(line, kCenterX, y);
}

void rebuildOrderByDistance() {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  s_order_count = n;
  for (size_t i = 0; i < n; ++i) {
    s_order[i] = static_cast<uint8_t>(i);
  }

  for (size_t i = 0; i + 1 < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      if (aircraftDistKm(planes[s_order[j]]) < aircraftDistKm(planes[s_order[i]])) {
        const uint8_t tmp = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = tmp;
      }
    }
  }
}

const services::adsb::Aircraft* selectedAircraft() {
  if (s_order_count == 0 || s_sel >= s_order_count) {
    return nullptr;
  }
  return &services::adsb::aircraftList()[s_order[s_sel]];
}

bool iataForIcao(const char* icao, char* iata_out) {
  if (icao == nullptr || icao[3] == '\0' || iata_out == nullptr) {
    return false;
  }
  for (size_t i = 0; i < data::airports::kIataCount; ++i) {
    data::airports::IataEntry entry;
    memcpy_P(&entry, &data::airports::kIataToIcao[i], sizeof(entry));
    if (entry.icao[0] == icao[0] && entry.icao[1] == icao[1] &&
        entry.icao[2] == icao[2] && entry.icao[3] == icao[3]) {
      iata_out[0] = entry.iata[0];
      iata_out[1] = entry.iata[1];
      iata_out[2] = entry.iata[2];
      iata_out[3] = '\0';
      return iata_out[0] != '\0';
    }
  }
  return false;
}

void routeDisplayCode(const char* route_code, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (route_code == nullptr || route_code[0] == '\0') {
    return;
  }

  char icao[5];
  if (services::airport::normalizeRouteCode(route_code, icao, sizeof(icao))) {
    char iata[4];
    if (iataForIcao(icao, iata)) {
      strncpy(out, iata, out_len - 1);
    } else {
      strncpy(out, icao, out_len - 1);
    }
    out[out_len - 1] = '\0';
    return;
  }

  strncpy(out, route_code, out_len - 1);
  out[out_len - 1] = '\0';
}

void formatRouteEndpointLabel(const char* route_code, char* out, size_t out_len) {
  if (out_len > 0) {
    out[0] = '\0';
  }
  if (route_code == nullptr || route_code[0] == '\0') {
    return;
  }

  char code[5];
  routeDisplayCode(route_code, code, sizeof(code));

  char name[data::airports::kMaxNameLen + 1];
  if (services::airport::lookupName(route_code, name, sizeof(name))) {
    snprintf(out, out_len, "%s, %s", code, name);
    return;
  }

  strncpy(out, code, out_len - 1);
  out[out_len - 1] = '\0';
}

void resolveRouteLabels(const services::adsb::Aircraft& ac, char* origin, size_t origin_len,
                        char* dest, size_t dest_len) {
  if (origin_len > 0) {
    origin[0] = '\0';
  }
  if (dest_len > 0) {
    dest[0] = '\0';
  }
  if (ac.route_origin[0] != '\0') {
    formatRouteEndpointLabel(ac.route_origin, origin, origin_len);
  }
  if (ac.route_dest[0] != '\0') {
    formatRouteEndpointLabel(ac.route_dest, dest, dest_len);
  }
}

int routeDisplayLines(const char* origin, const char* dest, int row_y, int row_h) {
  if ((origin == nullptr || origin[0] == '\0') &&
      (dest == nullptr || dest[0] == '\0')) {
    return 1;
  }
  if (origin == nullptr || origin[0] == '\0' || dest == nullptr || dest[0] == '\0') {
    return 1;
  }

  char one_line[(kRouteLabelLen + 1) * 2 + 8];
  snprintf(one_line, sizeof(one_line), "%s > %s", origin, dest);

  displayFontApply(tft, displayFontBody());
  const int max_w = circleHalfWidthAtRow(row_y, row_h) * 2;
  if (max_w <= 0) {
    return 2;
  }
  return tft.textWidth(one_line) <= max_w ? 1 : 2;
}

void drawRouteLabels(const char* origin, const char* dest, int* y, uint16_t fg, uint16_t bg) {
  const UiTextStyle style = displayFontBody();
  displayFontApply(tft, style);
  const int row_h = displayFontHeight(tft, style);

  if ((origin == nullptr || origin[0] == '\0') &&
      (dest == nullptr || dest[0] == '\0')) {
    drawCenterLine("Route unknown", y, style, fg, bg);
    return;
  }

  if (origin == nullptr || origin[0] == '\0') {
    char line[kRouteLabelLen + 8];
    snprintf(line, sizeof(line), "? > %s", dest);
    drawCenterLine(line, y, style, fg, bg);
    return;
  }

  if (dest == nullptr || dest[0] == '\0') {
    char line[kRouteLabelLen + 8];
    snprintf(line, sizeof(line), "%s > ?", origin);
    drawCenterLine(line, y, style, fg, bg);
    return;
  }

  char one_line[(kRouteLabelLen + 1) * 2 + 8];
  snprintf(one_line, sizeof(one_line), "%s > %s", origin, dest);
  const int max_w = circleHalfWidthAtRow(*y, row_h) * 2;
  if (max_w > 0 && tft.textWidth(one_line) <= max_w) {
    drawCenterLine(one_line, y, style, fg, bg);
    return;
  }

  drawCenterLine(origin, y, style, fg, bg);
  char dest_line[kRouteLabelLen + 8];
  snprintf(dest_line, sizeof(dest_line), "> %s", dest);
  drawCenterLine(dest_line, y, style, fg, bg);
}

void formatAltLine(const services::adsb::Aircraft& ac, char* out, size_t out_len) {
  if (ac.alt[0] != '\0') {
    snprintf(out, out_len, "Alt: %s", ac.alt);
  } else {
    strncpy(out, "Alt: —", out_len - 1);
    out[out_len - 1] = '\0';
  }
}

void formatSpeedLine(const services::adsb::Aircraft& ac, char* out, size_t out_len) {
  if (ac.gs_knots > 0.5f) {
    snprintf(out, out_len, "Speed: %d kt", static_cast<int>(lroundf(ac.gs_knots)));
  } else {
    strncpy(out, "Speed: —", out_len - 1);
    out[out_len - 1] = '\0';
  }
}

constexpr int kTypeMaxLines = 4;

struct FlightDetailStrings {
  char callsign[16];
  char airline[32];
  char route_origin[kRouteLabelLen + 1];
  char route_dest[kRouteLabelLen + 1];
  char type[data::icao_types::kMaxNameLen + 1];
  char alt[20];
  char speed[20];
  char index_line[16];
};

struct FlightDetailLayout {
  int y_start = 0;
  int y_alt = 0;
  int y_speed = 0;
};

struct FlightDetailSnapshot {
  bool valid = false;
  size_t sel = 0;
  size_t order_count = 0;
  FlightDetailStrings text = {};
  FlightDetailLayout layout = {};
};

FlightDetailSnapshot s_snapshot;

void populateFlightDetailStrings(const services::adsb::Aircraft& ac,
                                 FlightDetailStrings* out) {
  if (ac.callsign[0] != '\0') {
    strncpy(out->callsign, ac.callsign, sizeof(out->callsign) - 1);
    out->callsign[sizeof(out->callsign) - 1] = '\0';
  } else {
    strncpy(out->callsign, "—", sizeof(out->callsign) - 1);
  }

  if (ac.airline[0] != '\0') {
    strncpy(out->airline, ac.airline, sizeof(out->airline) - 1);
    out->airline[sizeof(out->airline) - 1] = '\0';
  } else {
    strncpy(out->airline, "Airline unknown", sizeof(out->airline) - 1);
  }

  resolveRouteLabels(ac, out->route_origin, sizeof(out->route_origin),
                     out->route_dest, sizeof(out->route_dest));
  formatTypeLine(ac, out->type, sizeof(out->type));
  formatAltLine(ac, out->alt, sizeof(out->alt));
  formatSpeedLine(ac, out->speed, sizeof(out->speed));
  snprintf(out->index_line, sizeof(out->index_line), "%u / %u",
           static_cast<unsigned>(s_sel + 1), static_cast<unsigned>(s_order_count));
}

void computeFlightDetailLayout(const FlightDetailStrings& s, FlightDetailLayout* layout) {
  const int title_h = displayFontHeight(tft, displayFontTitle());
  const int callsign_h = displayFontHeight(tft, displayFontBody());
  const int body_h = displayFontHeight(tft, displayFontBody());
  const int detail_h = displayFontHeight(tft, displayFontDetail());

  const int footer_h = kLineGap + detail_h + kLineGap + detail_h + kFooterGap + detail_h +
                       kLineGap + detail_h;

  const int route_block_est = body_h + kLineGap;
  const int pre_type_h_est = title_h + kTitleGap + callsign_h + kLineGap + body_h +
                             kSectionGap + route_block_est;

  int block_h = pre_type_h_est + footer_h + detail_h;
  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  const int route_y = y + title_h + kTitleGap + callsign_h + kLineGap + body_h + kSectionGap;
  const int route_lines =
      routeDisplayLines(s.route_origin, s.route_dest, route_y, body_h);
  const int route_block_h = route_lines * (body_h + kLineGap);

  const int pre_type_h_adj = title_h + kTitleGap + callsign_h + kLineGap + body_h +
                             kSectionGap + route_block_h;
  block_h = pre_type_h_adj + footer_h + detail_h;
  y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  const int type_y = y + pre_type_h_adj;
  const int type_lines =
      wrappedLineCount(s.type, displayFontDetail(), type_y, detail_h, kTypeMaxLines);
  const int type_block_h =
      type_lines * detail_h + (type_lines > 1 ? (type_lines - 1) * kLineGap : 0);

  block_h = pre_type_h_adj + type_block_h + footer_h;
  y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  int y_alt = y + pre_type_h_adj + type_block_h;
  layout->y_start = y;
  layout->y_alt = y_alt;
  layout->y_speed = y_alt + detail_h + kLineGap;
}

bool snapshotStaticMatches(const FlightDetailStrings& s,
                           const FlightDetailLayout& layout) {
  if (!s_snapshot.valid || s_sel != s_snapshot.sel ||
      s_order_count != s_snapshot.order_count) {
    return false;
  }
  if (layout.y_alt != s_snapshot.layout.y_alt ||
      layout.y_speed != s_snapshot.layout.y_speed) {
    return false;
  }
  return strcmp(s.callsign, s_snapshot.text.callsign) == 0 &&
         strcmp(s.airline, s_snapshot.text.airline) == 0 &&
         strcmp(s.route_origin, s_snapshot.text.route_origin) == 0 &&
         strcmp(s.route_dest, s_snapshot.text.route_dest) == 0 &&
         strcmp(s.type, s_snapshot.text.type) == 0;
}

void saveSnapshot(const FlightDetailStrings& s, const FlightDetailLayout& layout) {
  s_snapshot.valid = true;
  s_snapshot.sel = s_sel;
  s_snapshot.order_count = s_order_count;
  s_snapshot.text = s;
  s_snapshot.layout = layout;
}

void formatTypeLine(const services::adsb::Aircraft& ac, char* out, size_t out_len) {
  if (ac.type[0] == '\0') {
    strncpy(out, "—", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }
  if (services::aircraft_type::lookupDescription(ac.type, out, out_len)) {
    return;
  }
  strncpy(out, ac.type, out_len - 1);
  out[out_len - 1] = '\0';
}

}  // namespace

void flightDetailSelectClosest() {
  rebuildOrderByDistance();
  s_sel = 0;
  s_snapshot.valid = false;
}

void flightDetailSelectAtScreen(int16_t x, int16_t y) {
  rebuildOrderByDistance();
  if (s_order_count == 0) {
    s_snapshot.valid = false;
    return;
  }

  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  int best_i = 0;
  int best_d2 = INT32_MAX;
  const int pick_r2 = kTapPickRadiusPx * kTapPickRadiusPx;

  for (size_t i = 0; i < s_order_count; ++i) {
    const services::adsb::Aircraft& ac = planes[s_order[i]];
    int sx = 0;
    int sy = 0;
    latLonToScreen(ac.lat, ac.lon, &sx, &sy);
    const int dx = sx - x;
    const int dy = sy - y;
    const int d2 = dx * dx + dy * dy;
    if (d2 < best_d2) {
      best_d2 = d2;
      best_i = static_cast<int>(i);
    }
  }

  if (best_d2 <= pick_r2) {
    s_sel = static_cast<size_t>(best_i);
  } else {
    s_sel = 0;
  }
  s_snapshot.valid = false;
}

bool flightDetailCycle(int delta) {
  if (s_order_count == 0) {
    return false;
  }
  if (delta == 0) {
    return true;
  }
  const int n = static_cast<int>(s_order_count);
  int idx = static_cast<int>(s_sel) + delta;
  while (idx < 0) {
    idx += n;
  }
  while (idx >= n) {
    idx -= n;
  }
  s_sel = static_cast<size_t>(idx);
  s_snapshot.valid = false;
  return true;
}

void flightDetailDraw() {
  const uint16_t bg = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  const uint16_t route_fg = tft.color565(100, 220, 255);
  const uint16_t hint_fg = tft.color565(120, 140, 160);

  tft.fillScreen(bg);

  if (s_order_count == 0) {
    rebuildOrderByDistance();
  }

  const services::adsb::Aircraft* ac = selectedAircraft();
  if (ac == nullptr) {
    s_snapshot.valid = false;
    int y = kCenterY - 20;
    drawCenterLine("Flight", &y, displayFontTitle(), fg, bg);
    drawCenterLine("No aircraft", &y, displayFontBody(), label_fg, bg);
    drawCenterLine("Swipe right", &y, displayFontDetail(), hint_fg, bg);
    drawCenterLine("for radar", &y, displayFontDetail(), hint_fg, bg);
    tft.setTextDatum(TextDatum::TopLeft);
    return;
  }

  FlightDetailStrings s = {};
  FlightDetailLayout layout = {};
  populateFlightDetailStrings(*ac, &s);
  computeFlightDetailLayout(s, &layout);

  const int title_h = displayFontHeight(tft, displayFontTitle());

  int y = layout.y_start;
  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Flight", kCenterX, y);
  y += title_h + kTitleGap;

  drawCenterLine(s.callsign, &y, displayFontBody(), fg, bg);
  drawCenterLine(s.airline, &y, displayFontBody(), label_fg, bg);
  y += kSectionGap - kLineGap;
  drawRouteLabels(s.route_origin, s.route_dest, &y, route_fg, bg);
  drawCenterWrapped(s.type, &y, displayFontDetail(), label_fg, bg, kTypeMaxLines);
  drawCenterLine(s.alt, &y, displayFontDetail(), fg, bg);
  drawCenterLine(s.speed, &y, displayFontDetail(), fg, bg);

  y += kFooterGap;
  drawCenterLine(s.index_line, &y, displayFontDetail(), hint_fg, bg);
  drawCenterLine("Turn: next", &y, displayFontDetail(), hint_fg, bg);
  drawCenterLine("Swipe right", &y, displayFontDetail(), hint_fg, bg);

  tft.setTextDatum(TextDatum::TopLeft);
  saveSnapshot(s, layout);
}

void flightDetailRefresh() {
  if (s_order_count == 0) {
    rebuildOrderByDistance();
  }

  const services::adsb::Aircraft* ac = selectedAircraft();
  if (ac == nullptr) {
    flightDetailDraw();
    return;
  }

  FlightDetailStrings s = {};
  FlightDetailLayout layout = {};
  populateFlightDetailStrings(*ac, &s);
  computeFlightDetailLayout(s, &layout);

  if (!snapshotStaticMatches(s, layout)) {
    flightDetailDraw();
    return;
  }

  if (strcmp(s.alt, s_snapshot.text.alt) == 0 &&
      strcmp(s.speed, s_snapshot.text.speed) == 0) {
    return;
  }

  const uint16_t bg = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  const uint16_t fg = tft.color565(255, 255, 255);
  const UiTextStyle detail_style = displayFontDetail();

  if (strcmp(s.alt, s_snapshot.text.alt) != 0) {
    redrawCenterLineAt(layout.y_alt, s.alt, detail_style, fg, bg);
    strncpy(s_snapshot.text.alt, s.alt, sizeof(s_snapshot.text.alt) - 1);
    s_snapshot.text.alt[sizeof(s_snapshot.text.alt) - 1] = '\0';
  }
  if (strcmp(s.speed, s_snapshot.text.speed) != 0) {
    redrawCenterLineAt(layout.y_speed, s.speed, detail_style, fg, bg);
    strncpy(s_snapshot.text.speed, s.speed, sizeof(s_snapshot.text.speed) - 1);
    s_snapshot.text.speed[sizeof(s_snapshot.text.speed) - 1] = '\0';
  }
}

}  // namespace ui
