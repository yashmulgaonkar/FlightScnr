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
#include "services/route_lookup.h"
#include "services/airline_logo.h"
#include "services/airline_lookup.h"
#include "services/aircraft_photo.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace ui {

void flightDetailDraw();

namespace {

constexpr int kBezelInsetPx = 10;
constexpr int kTextPadPx = 6;
constexpr int kTitleGap = 4;
constexpr int kLogoGap = 3;
constexpr int kLogoTopMarginPx = 2;
constexpr int kPhotoCreditGap = 2;
constexpr int kPhotoBelowGap = 10;
constexpr int kLineGap = 3;
constexpr int kSectionGap = 6;
constexpr int kFooterGap = 6;
constexpr int kTapPickRadiusPx = 36;
constexpr size_t kRouteLabelLen = data::airports::kMaxNameLen + 6;  // "ICAO, " + name

PlaneGfxSprite s_detail_sprite(&tft);
bool s_detail_sprite_ready = false;

char s_last_draw_callsign[16] = "";
unsigned long s_last_draw_ms = 0;
bool s_last_draw_had_fetching = false;

bool ensureDetailSprite() {
  if (s_detail_sprite_ready) {
    return true;
  }
  if (!s_detail_sprite.createSprite(config::kDisplayWidth, config::kDisplayHeight)) {
    Serial.println("[detail] sprite alloc failed");
    return false;
  }
  s_detail_sprite_ready = true;
  return true;
}

PlaneGfx& detailGfx() { return s_detail_sprite.gfx(); }

void pushDetailToPanel() {
  if (s_detail_sprite_ready) {
    s_detail_sprite.pushSprite(0, 0);
  }
}

void blitDetailRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!s_detail_sprite_ready || w <= 0 || h <= 0) {
    return;
  }
  const int16_t stride = s_detail_sprite.width();
  tft.blitRegionFromBuffer(
      x, y, w, h,
      s_detail_sprite.buffer() + static_cast<size_t>(y) * static_cast<size_t>(stride) +
          static_cast<size_t>(x),
      stride);
}

void releaseDetailSprite() {
  if (s_detail_sprite_ready) {
    s_detail_sprite.deleteSprite();
    s_detail_sprite_ready = false;
  }
  services::airline::releaseLogoBuffer();
  // Keep Planespotters frame across detail-sprite TLS releases; cancel() frees it.
  s_last_draw_callsign[0] = '\0';
  s_last_draw_ms = 0;
  s_last_draw_had_fetching = false;
}

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

uint8_t s_order[services::adsb::kMaxAircraft];
size_t s_order_count = 0;
size_t s_sel = 0;
services::adsb::Aircraft s_planes[services::adsb::kMaxAircraft];
size_t s_plane_count = 0;

char s_recent_enrich_redraw_callsign[16] = "";
unsigned long s_recent_enrich_redraw_ms = 0;

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
  displayFontApply(detailGfx(), style);
  const int line_h = displayFontHeight(tft, style);
  const char* cursor = text;
  char line[64];

  detailGfx().setTextDatum(TextDatum::TopCenter);
  detailGfx().setTextColor(fg, bg);

  for (int i = 0; i < max_lines; ++i) {
    const int max_w = circleHalfWidthAtRow(*y, line_h) * 2;
    if (max_w <= 0) {
      break;
    }
    const bool last_line = i + 1 >= max_lines;
    if (!nextWrappedLine(&cursor, line, sizeof(line), style, max_w, last_line)) {
      break;
    }
    detailGfx().drawString(line, kCenterX, *y);
    *y += line_h + kLineGap;
    if (*cursor == '\0') {
      break;
    }
  }
}

void drawCenterLine(const char* text, int* y, UiTextStyle style, uint16_t fg,
                    uint16_t bg) {
  displayFontApply(detailGfx(), style);
  const int h = displayFontHeight(tft, style);
  const int max_half_w = circleHalfWidthAtRow(*y, h);
  const int max_w = max_half_w * 2;

  char line[48];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  fitLineToWidth(line, sizeof(line), style, max_w);

  detailGfx().setTextDatum(TextDatum::TopCenter);
  detailGfx().setTextColor(fg, bg);
  detailGfx().drawString(line, kCenterX, *y);
  *y += h + kLineGap;
}

void redrawCenterLineAt(int y, const char* text, UiTextStyle style, uint16_t fg,
                        uint16_t bg) {
  if (!ensureDetailSprite()) {
    return;
  }
  displayFontApply(detailGfx(), style);
  const int h = displayFontHeight(tft, style);
  const int max_half_w = circleHalfWidthAtRow(y, h);
  const int max_w = max_half_w * 2;

  char line[48];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  fitLineToWidth(line, sizeof(line), style, max_w);

  if (max_w > 0) {
    detailGfx().fillRect(kCenterX - max_half_w, y, max_w, h, bg);
  }
  detailGfx().setTextDatum(TextDatum::TopCenter);
  detailGfx().setTextColor(fg, bg);
  detailGfx().drawString(line, kCenterX, y);
  if (max_w > 0) {
    blitDetailRegion(static_cast<int16_t>(kCenterX - max_half_w), static_cast<int16_t>(y),
                     static_cast<int16_t>(max_w), static_cast<int16_t>(h));
  }
}

void rebuildOrderByDistance() {
  s_plane_count = services::adsb::copyAircraftSnapshot(s_planes, services::adsb::kMaxAircraft);
  const size_t n = s_plane_count;
  s_order_count = n;
  for (size_t i = 0; i < n; ++i) {
    s_order[i] = static_cast<uint8_t>(i);
  }

  for (size_t i = 0; i + 1 < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      if (aircraftDistKm(s_planes[s_order[j]]) < aircraftDistKm(s_planes[s_order[i]])) {
        const uint8_t tmp = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = tmp;
      }
    }
  }
}

bool orderEntryInBounds(size_t order_idx) {
  return order_idx < s_order_count && s_order[order_idx] < s_plane_count;
}

void resyncOrderPreservingSelection() {
  char keep_callsign[sizeof(services::adsb::Aircraft::callsign)] = {};
  if (orderEntryInBounds(s_sel)) {
    const services::adsb::Aircraft& ac = s_planes[s_order[s_sel]];
    if (ac.callsign[0] != '\0') {
      strncpy(keep_callsign, ac.callsign, sizeof(keep_callsign) - 1);
      keep_callsign[sizeof(keep_callsign) - 1] = '\0';
    }
  }

  rebuildOrderByDistance();

  if (keep_callsign[0] != '\0') {
    for (size_t i = 0; i < s_order_count; ++i) {
      if (!orderEntryInBounds(i)) {
        continue;
      }
      const services::adsb::Aircraft& ac = s_planes[s_order[i]];
      if (strcmp(ac.callsign, keep_callsign) == 0) {
        s_sel = i;
        if (config::kSerialTraceDebug) {
          Serial.printf("[detail] resync kept %s sel=%u/%u\n", keep_callsign,
                        static_cast<unsigned>(s_sel + 1),
                        static_cast<unsigned>(s_order_count));
        }
        return;
      }
    }
    if (config::kSerialTraceDebug) {
      Serial.printf("[detail] resync lost %s (off list)\n", keep_callsign);
    }
    s_sel = 0;
  }

  if (s_sel >= s_order_count && s_order_count > 0) {
    s_sel = 0;
  }
  if (keep_callsign[0] == '\0' && config::kSerialTraceDebug) {
    Serial.printf("[detail] resync planes=%u sel=%u\n",
                  static_cast<unsigned>(s_order_count),
                  static_cast<unsigned>(s_sel + 1));
  }
}

bool copySelectedAircraft(services::adsb::Aircraft* out) {
  if (out == nullptr || !orderEntryInBounds(s_sel)) {
    return false;
  }
  *out = s_planes[s_order[s_sel]];
  return true;
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
  displayFontApply(detailGfx(), style);
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

void formatAltSpeedLine(const services::adsb::Aircraft& ac, char* out, size_t out_len) {
  char alt_display[20];
  radar::formatAltitudeDisplay(ac.alt, alt_display, sizeof(alt_display));
  if (alt_display[0] == '\0') {
    strncpy(alt_display, "—", sizeof(alt_display) - 1);
    alt_display[sizeof(alt_display) - 1] = '\0';
  }

  char speed_display[20];
  if (ac.gs_knots <= 0.5f) {
    strncpy(speed_display, "—", sizeof(speed_display) - 1);
    speed_display[sizeof(speed_display) - 1] = '\0';
  } else {
    switch (radar::distanceUnit()) {
      case radar::DistanceUnit::Km:
        snprintf(speed_display, sizeof(speed_display), "%d km/h",
                 static_cast<int>(lroundf(ac.gs_knots * radar::kKnotsToKmh)));
        break;
      case radar::DistanceUnit::StatuteMile:
        snprintf(speed_display, sizeof(speed_display), "%d mph",
                 static_cast<int>(lroundf(ac.gs_knots * radar::kKnotsToMph)));
        break;
      default:
        snprintf(speed_display, sizeof(speed_display), "%d kt",
                 static_cast<int>(lroundf(ac.gs_knots)));
        break;
    }
  }

  snprintf(out, out_len, "%s, %s", alt_display, speed_display);
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

constexpr int kTypeMaxLines = 4;
constexpr char kFetchingDetails[] = "Fetching Details";
constexpr char kNoApisEnabled[] = "No APIs Enabled";
constexpr char kAirlineUnknown[] = "Airline unknown";
constexpr char kRouteUnknown[] = "Route unknown";
constexpr unsigned long kNoApisAlternateMs = 1000UL;

enum class EnrichFieldPlaceholder : uint8_t { None, Fetching };

bool alternateNoApisAirline(const services::adsb::Aircraft& ac) {
  return !services::route::liveRouteApiAvailable() && ac.airline[0] == '\0';
}

bool alternateNoApisRoute(const services::adsb::Aircraft& ac) {
  return !services::route::liveRouteApiAvailable() && ac.route_origin[0] == '\0' &&
         ac.route_dest[0] == '\0';
}

uint8_t noApisAlternatePhase(unsigned long now_ms) {
  return static_cast<uint8_t>((now_ms / kNoApisAlternateMs) % 2UL);
}

EnrichFieldPlaceholder enrichAirlinePlaceholder(const services::adsb::Aircraft& ac) {
  if (ac.airline[0] != '\0' || alternateNoApisAirline(ac)) {
    return EnrichFieldPlaceholder::None;
  }
  if (services::route::liveRouteApiAvailable() &&
      services::route::detailEnrichmentInFlight(ac.callsign)) {
    return EnrichFieldPlaceholder::Fetching;
  }
  return EnrichFieldPlaceholder::None;
}

EnrichFieldPlaceholder enrichRoutePlaceholder(const services::adsb::Aircraft& ac) {
  if (ac.route_origin[0] != '\0' || ac.route_dest[0] != '\0' || alternateNoApisRoute(ac)) {
    return EnrichFieldPlaceholder::None;
  }
  if (services::route::liveRouteApiAvailable() &&
      services::route::detailEnrichmentInFlight(ac.callsign)) {
    return EnrichFieldPlaceholder::Fetching;
  }
  return EnrichFieldPlaceholder::None;
}

uint16_t fetchingDetailColor() { return tft.color565(255, 200, 0); }

uint16_t noApisDetailColor() { return tft.color565(255, 80, 80); }

const char* alternateNoApisAirlineText(unsigned long now_ms) {
  return noApisAlternatePhase(now_ms) == 0 ? kNoApisEnabled : kAirlineUnknown;
}

const char* alternateNoApisRouteText(unsigned long now_ms) {
  return noApisAlternatePhase(now_ms) == 0 ? kNoApisEnabled : kRouteUnknown;
}

uint16_t alternateNoApisAirlineColor(unsigned long now_ms, uint16_t route_fg) {
  return noApisAlternatePhase(now_ms) == 0 ? noApisDetailColor() : route_fg;
}

uint16_t alternateNoApisRouteColor(unsigned long now_ms, uint16_t route_fg) {
  return noApisAlternatePhase(now_ms) == 0 ? noApisDetailColor() : route_fg;
}

void drawEnrichPlaceholder(const char* text, int* y, UiTextStyle style,
                           EnrichFieldPlaceholder placeholder, uint16_t bg) {
  if (placeholder != EnrichFieldPlaceholder::Fetching || text[0] == '\0') {
    return;
  }
  drawCenterLine(text, y, style, fetchingDetailColor(), bg);
}

struct FlightDetailStrings {
  char callsign[16];
  char airline[32];
  char route_origin[kRouteLabelLen + 1];
  char route_dest[kRouteLabelLen + 1];
  char type[data::icao_types::kMaxNameLen + 1];
  char alt[28];
  char speed[24];
  char index_line[16];
};

struct FlightDetailLayout {
  int y_start = 0;
  int y_logo = 0;
  int y_photo = 0;
  int y_photo_credit = 0;
  int y_airline = 0;
  int y_route = 0;
  int y_alt = 0;
  int y_speed = 0;
  int logo_h = 0;
  int photo_h = 0;
  int photo_credit_h = 0;
};

struct FlightDetailSnapshot {
  bool valid = false;
  size_t sel = 0;
  size_t order_count = 0;
  EnrichFieldPlaceholder airline_placeholder = EnrichFieldPlaceholder::None;
  EnrichFieldPlaceholder route_placeholder = EnrichFieldPlaceholder::None;
  bool alternate_no_apis_airline = false;
  bool alternate_no_apis_route = false;
  uint8_t no_apis_alt_phase = 0;
  FlightDetailStrings text = {};
  FlightDetailLayout layout = {};
};

FlightDetailSnapshot s_snapshot;
FlightDetailStrings s_draw_strings;
FlightDetailLayout s_draw_layout;

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
  formatAltSpeedLine(ac, out->alt, sizeof(out->alt));
  out->speed[0] = '\0';
  snprintf(out->index_line, sizeof(out->index_line), "%u / %u",
           static_cast<unsigned>(s_sel + 1), static_cast<unsigned>(s_order_count));
}

void resolveAirlineIcao(const services::adsb::Aircraft& ac, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (ac.airline_icao[0] != '\0') {
    strncpy(out, ac.airline_icao, out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }
  services::airline::resolveIcaoFromCallsign(ac.callsign, ac.callsign[0] != '\0', out,
                                             out_len);
}

int layoutBottomY(int block_h) {
  const int bottom_limit = kCenterY + kCircleRadius - kBezelInsetPx;
  int y = bottom_limit - block_h;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }
  return y;
}

int layoutCenterY(int block_h) {
  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }
  const int bottom_limit = kCenterY + kCircleRadius - kBezelInsetPx;
  if (y + block_h > bottom_limit) {
    y = bottom_limit - block_h;
    if (y < kBezelInsetPx) {
      y = kBezelInsetPx;
    }
  }
  return y;
}

void computeFlightDetailLayout(const FlightDetailStrings& s, int logo_h, int photo_h,
                               int photo_credit_h, EnrichFieldPlaceholder route_placeholder,
                               bool alternate_no_apis_route, FlightDetailLayout* layout) {
  const int callsign_h = displayFontHeight(tft, displayFontBody());
  const int body_h = displayFontHeight(tft, displayFontBody());
  const int detail_h = displayFontHeight(tft, displayFontDetail());
  const int metrics_h = detail_h;
  const int footer_h = kFooterGap + detail_h;
  const int top = kCenterY - kCircleRadius + kLogoTopMarginPx;

  auto refineTextMetrics = [&](int y_text, int* out_pre_type_h, int* out_type_block_h,
                               int* out_text_h) {
    const int route_y = y_text + callsign_h + kLineGap + body_h + kSectionGap;
    const int route_lines =
        (route_placeholder != EnrichFieldPlaceholder::None || alternate_no_apis_route)
            ? 1
            : routeDisplayLines(s.route_origin, s.route_dest, route_y, body_h);
    const int route_block_h = route_lines * (body_h + kLineGap);
    const int pre_type_h = callsign_h + kLineGap + body_h + kSectionGap + route_block_h;
    const int type_y = y_text + pre_type_h;
    const int type_lines =
        wrappedLineCount(s.type, displayFontDetail(), type_y, detail_h, kTypeMaxLines);
    const int type_block_h =
        type_lines * detail_h + (type_lines > 1 ? (type_lines - 1) * kLineGap : 0);
    *out_pre_type_h = pre_type_h;
    *out_type_block_h = type_block_h;
    *out_text_h = pre_type_h + type_block_h + kLineGap + metrics_h + footer_h;
  };

  layout->logo_h = 0;
  layout->photo_h = 0;
  layout->photo_credit_h = 0;
  layout->y_photo = top;
  layout->y_photo_credit = top;
  layout->y_logo = top;

  int pre_type_h = 0;
  int type_block_h = 0;
  int text_h = 0;
  int y_start = 0;

  if (photo_h > 0) {
    // Photo present: pin image at top, text at bottom (grows upward).
    layout->photo_h = photo_h;
    layout->photo_credit_h = photo_credit_h;
    const int art_h =
        photo_h + (photo_credit_h > 0 ? kPhotoCreditGap + photo_credit_h : 0);
    layout->y_photo = top;
    layout->y_photo_credit = layout->y_photo + photo_h + kPhotoCreditGap;

    y_start = layoutBottomY(callsign_h + kLineGap + body_h + kSectionGap + body_h +
                            kLineGap + detail_h + kLineGap + metrics_h + footer_h);
    refineTextMetrics(y_start, &pre_type_h, &type_block_h, &text_h);
    const int y_bottom = layoutBottomY(text_h);
    const int min_text_y = top + art_h + kPhotoBelowGap;
    y_start = y_bottom < min_text_y ? min_text_y : y_bottom;
    refineTextMetrics(y_start, &pre_type_h, &type_block_h, &text_h);
  } else {
    // No photo yet: keep logo + text as one centered block (no empty mid gap).
    const int art_h = logo_h > 0 ? logo_h + kLogoGap : 0;
    int y_block = layoutCenterY(art_h + callsign_h + kLineGap + body_h + kSectionGap +
                                body_h + kLineGap + detail_h + kLineGap + metrics_h +
                                footer_h);
    y_start = y_block + art_h;
    refineTextMetrics(y_start, &pre_type_h, &type_block_h, &text_h);
    const int total_h = art_h + text_h;
    y_block = layoutCenterY(total_h);
    if (logo_h > 0) {
      layout->logo_h = logo_h;
      layout->y_logo = y_block;
      y_start = y_block + logo_h + kLogoGap;
    } else {
      y_start = y_block;
    }
    refineTextMetrics(y_start, &pre_type_h, &type_block_h, &text_h);
  }

  layout->y_start = y_start;
  layout->y_airline = layout->y_start + callsign_h + kLineGap;
  layout->y_route = layout->y_airline + body_h + kSectionGap;
  layout->y_alt = layout->y_start + pre_type_h + type_block_h + kLineGap;
  layout->y_speed = layout->y_alt;
}

bool snapshotStaticMatches(const FlightDetailStrings& s, const FlightDetailLayout& layout,
                           EnrichFieldPlaceholder airline_placeholder,
                           EnrichFieldPlaceholder route_placeholder,
                           bool alternate_no_apis_airline, bool alternate_no_apis_route,
                           uint8_t no_apis_alt_phase) {
  (void)no_apis_alt_phase;
  if (!s_snapshot.valid) {
    return false;
  }
  if (layout.logo_h != s_snapshot.layout.logo_h ||
      layout.y_logo != s_snapshot.layout.y_logo ||
      layout.photo_h != s_snapshot.layout.photo_h ||
      layout.y_photo != s_snapshot.layout.y_photo ||
      layout.y_alt != s_snapshot.layout.y_alt ||
      layout.y_speed != s_snapshot.layout.y_speed) {
    return false;
  }
  if (airline_placeholder != s_snapshot.airline_placeholder ||
      route_placeholder != s_snapshot.route_placeholder) {
    return false;
  }
  if (alternate_no_apis_airline != s_snapshot.alternate_no_apis_airline ||
      alternate_no_apis_route != s_snapshot.alternate_no_apis_route) {
    return false;
  }
  if (strcmp(s.callsign, s_snapshot.text.callsign) != 0 ||
      strcmp(s.type, s_snapshot.text.type) != 0) {
    return false;
  }
  if (alternate_no_apis_airline || alternate_no_apis_route) {
    return true;
  }
  if (airline_placeholder == EnrichFieldPlaceholder::Fetching ||
      route_placeholder == EnrichFieldPlaceholder::Fetching) {
    return true;
  }
  return strcmp(s.airline, s_snapshot.text.airline) == 0 &&
         strcmp(s.route_origin, s_snapshot.text.route_origin) == 0 &&
         strcmp(s.route_dest, s_snapshot.text.route_dest) == 0;
}

void updateSnapshotSelectionIndex(const FlightDetailStrings& s) {
  if (!s_snapshot.valid) {
    return;
  }
  s_snapshot.sel = s_sel;
  strncpy(s_snapshot.text.index_line, s.index_line, sizeof(s_snapshot.text.index_line) - 1);
  s_snapshot.text.index_line[sizeof(s_snapshot.text.index_line) - 1] = '\0';
}

void saveSnapshot(const FlightDetailStrings& s, const FlightDetailLayout& layout,
                  EnrichFieldPlaceholder airline_placeholder,
                  EnrichFieldPlaceholder route_placeholder,
                  bool alternate_no_apis_airline, bool alternate_no_apis_route,
                  uint8_t no_apis_alt_phase) {
  s_snapshot.valid = true;
  s_snapshot.sel = s_sel;
  s_snapshot.order_count = s_order_count;
  s_snapshot.airline_placeholder = airline_placeholder;
  s_snapshot.route_placeholder = route_placeholder;
  s_snapshot.alternate_no_apis_airline = alternate_no_apis_airline;
  s_snapshot.alternate_no_apis_route = alternate_no_apis_route;
  s_snapshot.no_apis_alt_phase = no_apis_alt_phase;
  s_snapshot.text = s;
  s_snapshot.layout = layout;
}

void tickNoApisAlternateLabels(unsigned long now_ms) {
  if (!s_snapshot.valid) {
    return;
  }
  if (!s_snapshot.alternate_no_apis_airline && !s_snapshot.alternate_no_apis_route) {
    return;
  }

  const uint8_t phase = noApisAlternatePhase(now_ms);
  if (phase == s_snapshot.no_apis_alt_phase) {
    return;
  }

  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t route_fg = tft.color565(100, 220, 255);
  const UiTextStyle body_style = displayFontBody();

  if (s_snapshot.alternate_no_apis_airline) {
    const char* text = alternateNoApisAirlineText(now_ms);
    redrawCenterLineAt(s_snapshot.layout.y_airline, text, body_style,
                       alternateNoApisAirlineColor(now_ms, route_fg), bg);
    strncpy(s_snapshot.text.airline, text, sizeof(s_snapshot.text.airline) - 1);
    s_snapshot.text.airline[sizeof(s_snapshot.text.airline) - 1] = '\0';
  }
  if (s_snapshot.alternate_no_apis_route) {
    const char* text = alternateNoApisRouteText(now_ms);
    redrawCenterLineAt(s_snapshot.layout.y_route, text, body_style,
                       alternateNoApisRouteColor(now_ms, route_fg), bg);
    strncpy(s_snapshot.text.route_origin, text, sizeof(s_snapshot.text.route_origin) - 1);
    s_snapshot.text.route_origin[sizeof(s_snapshot.text.route_origin) - 1] = '\0';
    s_snapshot.text.route_dest[0] = '\0';
  }
  s_snapshot.no_apis_alt_phase = phase;
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

  int best_i = 0;
  int best_d2 = INT32_MAX;
  const int pick_r2 = kTapPickRadiusPx * kTapPickRadiusPx;

  for (size_t i = 0; i < s_order_count; ++i) {
    if (!orderEntryInBounds(i)) {
      continue;
    }
    const services::adsb::Aircraft& ac = s_planes[s_order[i]];
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
  resyncOrderPreservingSelection();
  s_snapshot.valid = false;
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
  if (config::kSerialTraceDebug) {
    const char* cs = flightDetailSelectedCallsign();
    Serial.printf("[detail] cycle sel=%u/%u callsign=%s\n",
                  static_cast<unsigned>(s_sel + 1), static_cast<unsigned>(s_order_count),
                  cs != nullptr ? cs : "(none)");
  }
  return true;
}

const char* flightDetailSelectedCallsign() {
  static services::adsb::Aircraft ac;
  if (!copySelectedAircraft(&ac) || ac.callsign[0] == '\0') {
    return nullptr;
  }
  return ac.callsign;
}

const char* flightDetailSelectedHex() {
  static services::adsb::Aircraft ac;
  if (!copySelectedAircraft(&ac) || ac.hex[0] == '\0') {
    return nullptr;
  }
  return ac.hex;
}

void flightDetailDraw() {
  const unsigned long draw_start_ms = millis();
  if (services::route::detailDrawUnsafe()) {
    return;
  }
  if (!ensureDetailSprite()) {
    return;
  }
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  const uint16_t route_fg = tft.color565(100, 220, 255);
  const uint16_t hint_fg = tft.color565(120, 140, 160);

  resyncOrderPreservingSelection();

  services::adsb::Aircraft ac = {};
  if (!copySelectedAircraft(&ac)) {
    detailGfx().fillScreen(bg);
    s_snapshot.valid = false;
    int y = kCenterY - 20;
    drawCenterLine("No aircraft", &y, displayFontBody(), label_fg, bg);
    drawCenterLine("Swipe right", &y, displayFontDetail(), hint_fg, bg);
    drawCenterLine("for radar", &y, displayFontDetail(), hint_fg, bg);
    detailGfx().setTextDatum(TextDatum::TopLeft);
    pushDetailToPanel();
    if (config::kSerialTraceDebug) {
      Serial.printf("[detail] draw empty (%lums)\n", millis() - draw_start_ms);
    }
    return;
  }

  FlightDetailStrings& s = s_draw_strings;
  FlightDetailLayout& layout = s_draw_layout;
  s = {};
  layout = {};
  populateFlightDetailStrings(ac, &s);
  const EnrichFieldPlaceholder airline_placeholder = enrichAirlinePlaceholder(ac);
  const EnrichFieldPlaceholder route_placeholder = enrichRoutePlaceholder(ac);
  const bool alternate_airline = alternateNoApisAirline(ac);
  const bool alternate_route = alternateNoApisRoute(ac);
  const unsigned long now_ms = millis();
  const uint8_t alt_phase = noApisAlternatePhase(now_ms);
  char airline_icao[4] = {};
  resolveAirlineIcao(ac, airline_icao, sizeof(airline_icao));
  const int photo_h = services::photo::imageHeight(ac.callsign);
  const char* photographer = services::photo::photographer(ac.callsign);
  const int photo_credit_h =
      (photo_h > 0 && photographer != nullptr && photographer[0] != '\0')
          ? displayFontHeight(tft, displayFontDetail())
          : 0;
  // Prefer airframe photo over airline logo when available.
  const int logo_h =
      photo_h > 0 ? 0 : services::airline::logoHeightForIcao(airline_icao);
  computeFlightDetailLayout(s, logo_h, photo_h, photo_credit_h, route_placeholder,
                            alternate_route, &layout);

  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] draw begin %s layout=%dms\n", s.callsign,
                  static_cast<int>(millis() - draw_start_ms));
  }

  detailGfx().fillScreen(bg);

  if (layout.photo_h > 0) {
    services::photo::draw(detailGfx(), ac.callsign, kCenterX, layout.y_photo);
    if (layout.photo_credit_h > 0) {
      char credit[48];
      snprintf(credit, sizeof(credit), "© %s", photographer);
      int credit_y = layout.y_photo_credit;
      drawCenterLine(credit, &credit_y, displayFontDetail(), hint_fg, bg);
    }
  } else if (layout.logo_h > 0 && airline_icao[0] != '\0') {
    services::airline::drawLogo(detailGfx(), airline_icao, kCenterX, layout.y_logo, bg);
  }

  int y = layout.y_start;
  drawCenterLine(s.callsign, &y, displayFontBody(), fg, bg);
  if (airline_placeholder == EnrichFieldPlaceholder::Fetching) {
    drawEnrichPlaceholder(kFetchingDetails, &y, displayFontBody(), airline_placeholder, bg);
  } else if (alternate_airline) {
    drawCenterLine(alternateNoApisAirlineText(now_ms), &y, displayFontBody(),
                   alternateNoApisAirlineColor(now_ms, route_fg), bg);
  } else {
    drawCenterLine(s.airline, &y, displayFontBody(), label_fg, bg);
  }
  y += kSectionGap - kLineGap;
  if (route_placeholder == EnrichFieldPlaceholder::Fetching) {
    drawEnrichPlaceholder(kFetchingDetails, &y, displayFontBody(), route_placeholder, bg);
  } else if (alternate_route) {
    drawCenterLine(alternateNoApisRouteText(now_ms), &y, displayFontBody(),
                   alternateNoApisRouteColor(now_ms, route_fg), bg);
  } else {
    drawRouteLabels(s.route_origin, s.route_dest, &y, route_fg, bg);
  }
  drawCenterWrapped(s.type, &y, displayFontDetail(), label_fg, bg, kTypeMaxLines);
  drawCenterLine(s.alt, &y, displayFontDetail(), fg, bg);

  y += kFooterGap;
  drawCenterLine(s.index_line, &y, displayFontDetail(), hint_fg, bg);

  detailGfx().setTextDatum(TextDatum::TopLeft);
  if (airline_placeholder == EnrichFieldPlaceholder::Fetching) {
    strncpy(s.airline, kFetchingDetails, sizeof(s.airline) - 1);
    s.airline[sizeof(s.airline) - 1] = '\0';
  } else if (alternate_airline) {
    strncpy(s.airline, alternateNoApisAirlineText(now_ms), sizeof(s.airline) - 1);
    s.airline[sizeof(s.airline) - 1] = '\0';
  }
  if (route_placeholder == EnrichFieldPlaceholder::Fetching) {
    strncpy(s.route_origin, kFetchingDetails, sizeof(s.route_origin) - 1);
    s.route_origin[sizeof(s.route_origin) - 1] = '\0';
    s.route_dest[0] = '\0';
  } else if (alternate_route) {
    strncpy(s.route_origin, alternateNoApisRouteText(now_ms), sizeof(s.route_origin) - 1);
    s.route_origin[sizeof(s.route_origin) - 1] = '\0';
    s.route_dest[0] = '\0';
  }
  saveSnapshot(s, layout, airline_placeholder, route_placeholder, alternate_airline,
                 alternate_route, alt_phase);
  pushDetailToPanel();
  strncpy(s_last_draw_callsign, s.callsign, sizeof(s_last_draw_callsign) - 1);
  s_last_draw_callsign[sizeof(s_last_draw_callsign) - 1] = '\0';
  s_last_draw_ms = millis();
  s_last_draw_had_fetching =
      airline_placeholder == EnrichFieldPlaceholder::Fetching ||
      route_placeholder == EnrichFieldPlaceholder::Fetching;
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] draw done %s (%lums)\n", s.callsign,
                  millis() - draw_start_ms);
  }
}

void flightDetailRefresh() {
  if (services::route::detailDrawUnsafe()) {
    return;
  }
  resyncOrderPreservingSelection();

  services::adsb::Aircraft ac = {};
  if (!copySelectedAircraft(&ac)) {
    flightDetailDraw();
    return;
  }

  FlightDetailStrings& s = s_draw_strings;
  FlightDetailLayout& layout = s_draw_layout;
  s = {};
  layout = {};
  populateFlightDetailStrings(ac, &s);
  const EnrichFieldPlaceholder airline_placeholder = enrichAirlinePlaceholder(ac);
  const EnrichFieldPlaceholder route_placeholder = enrichRoutePlaceholder(ac);
  const bool alternate_airline = alternateNoApisAirline(ac);
  const bool alternate_route = alternateNoApisRoute(ac);
  const uint8_t alt_phase = noApisAlternatePhase(millis());
  char airline_icao[4] = {};
  resolveAirlineIcao(ac, airline_icao, sizeof(airline_icao));
  const int photo_h = services::photo::imageHeight(ac.callsign);
  const char* photographer = services::photo::photographer(ac.callsign);
  const int photo_credit_h =
      (photo_h > 0 && photographer != nullptr && photographer[0] != '\0')
          ? displayFontHeight(tft, displayFontDetail())
          : 0;
  const int logo_h =
      photo_h > 0 ? 0 : services::airline::logoHeightForIcao(airline_icao);
  computeFlightDetailLayout(s, logo_h, photo_h, photo_credit_h, route_placeholder,
                            alternate_route, &layout);

  if (!snapshotStaticMatches(s, layout, airline_placeholder, route_placeholder,
                             alternate_airline, alternate_route, alt_phase)) {
    if (flightDetailRecentlyRedrawn(s.callsign, 400UL) ||
        flightDetailRecentlyShowedRoute(s.callsign, 400UL)) {
      const bool index_changed = strcmp(s.index_line, s_snapshot.text.index_line) != 0;
      const bool alt_changed = strcmp(s.alt, s_snapshot.text.alt) != 0;
      updateSnapshotSelectionIndex(s);
      s_snapshot.order_count = s_order_count;
      if (!index_changed && !alt_changed) {
        return;
      }
      if (!ensureDetailSprite()) {
        flightDetailDraw();
        return;
      }
      const uint16_t bg = tft.color565(0, 0, 0);
      const uint16_t fg = tft.color565(255, 255, 255);
      const uint16_t hint_fg = tft.color565(120, 140, 160);
      const UiTextStyle detail_style = displayFontDetail();
      if (index_changed) {
        const int detail_h = displayFontHeight(tft, detail_style);
        const int y_index = s_snapshot.layout.y_alt + detail_h + kFooterGap;
        redrawCenterLineAt(y_index, s.index_line, detail_style, hint_fg, bg);
      }
      if (alt_changed) {
        redrawCenterLineAt(s_snapshot.layout.y_alt, s.alt, detail_style, fg, bg);
        strncpy(s_snapshot.text.alt, s.alt, sizeof(s_snapshot.text.alt) - 1);
        s_snapshot.text.alt[sizeof(s_snapshot.text.alt) - 1] = '\0';
      }
      return;
    }
    if (config::kSerialTraceDebug) {
      Serial.printf("[detail] refresh static changed %s -> full draw\n", s.callsign);
    }
    if (services::route::detailDrawUnsafe()) {
      return;
    }
    flightDetailDraw();
    return;
  }

  const bool index_changed = strcmp(s.index_line, s_snapshot.text.index_line) != 0;
  const bool alt_changed = strcmp(s.alt, s_snapshot.text.alt) != 0;
  updateSnapshotSelectionIndex(s);
  s_snapshot.order_count = s_order_count;

  if (!index_changed && !alt_changed) {
    return;
  }

  if (!ensureDetailSprite()) {
    flightDetailDraw();
    return;
  }

  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t hint_fg = tft.color565(120, 140, 160);
  const UiTextStyle detail_style = displayFontDetail();

  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] refresh partial %s%s%s\n", s.callsign,
                  index_changed ? " index" : "",
                  alt_changed ? " alt" : "");
  }

  if (index_changed) {
    const int detail_h = displayFontHeight(tft, detail_style);
    const int y_index = s_snapshot.layout.y_alt + detail_h + kFooterGap;
    redrawCenterLineAt(y_index, s.index_line, detail_style, hint_fg, bg);
  }
  if (alt_changed) {
    redrawCenterLineAt(s_snapshot.layout.y_alt, s.alt, detail_style, fg, bg);
    strncpy(s_snapshot.text.alt, s.alt, sizeof(s_snapshot.text.alt) - 1);
    s_snapshot.text.alt[sizeof(s_snapshot.text.alt) - 1] = '\0';
  }
}

void flightDetailTick(unsigned long now_ms) { tickNoApisAlternateLabels(now_ms); }

void flightDetailReleaseSprite() {
  s_recent_enrich_redraw_callsign[0] = '\0';
  s_recent_enrich_redraw_ms = 0;
  releaseDetailSprite();
}

bool flightDetailSpriteReady() { return s_detail_sprite_ready; }

void flightDetailMarkEnrichRedrawn(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0') {
    s_recent_enrich_redraw_callsign[0] = '\0';
    s_recent_enrich_redraw_ms = 0;
    return;
  }
  strncpy(s_recent_enrich_redraw_callsign, callsign, sizeof(s_recent_enrich_redraw_callsign) - 1);
  s_recent_enrich_redraw_callsign[sizeof(s_recent_enrich_redraw_callsign) - 1] = '\0';
  s_recent_enrich_redraw_ms = millis();
}

bool flightDetailRecentlyRedrawn(const char* callsign, unsigned long window_ms) {
  if (s_recent_enrich_redraw_ms == 0) {
    return false;
  }
  if (millis() - s_recent_enrich_redraw_ms >= window_ms) {
    return false;
  }
  if (callsign == nullptr || callsign[0] == '\0') {
    return true;
  }
  return strcmp(callsign, s_recent_enrich_redraw_callsign) == 0;
}

bool flightDetailRecentlyShowedRoute(const char* callsign, unsigned long window_ms) {
  if (s_last_draw_ms == 0 || s_last_draw_had_fetching) {
    return false;
  }
  if (millis() - s_last_draw_ms >= window_ms) {
    return false;
  }
  if (callsign == nullptr || callsign[0] == '\0') {
    return true;
  }
  return strcmp(callsign, s_last_draw_callsign) == 0;
}

}  // namespace ui
