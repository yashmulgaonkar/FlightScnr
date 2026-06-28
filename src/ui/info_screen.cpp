#include "ui/info_screen.h"

#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/buzzer.h"
#include "hardware/display.h"
#include "hardware/display_brightness.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/api_keys.h"
#include "services/map_center.h"
#include "ui/display_prefs.h"
#include "ui/radar_accent.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace {

constexpr int kBezelInsetPx = 10;
constexpr int kTextPadPx = 6;
constexpr int kTitleGap = 6;
constexpr int kLineGap = 4;
constexpr int kFooterGap = 8;
constexpr int kSectionGap = 6;
/** Space between main content and help hints. */
constexpr int kHintsTopGap = 22;

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

ui::InfoSettingsPage s_page = ui::InfoSettingsPage::Main;

enum class DisplayAdjustRow : uint8_t {
  Brightness,
  Units,
  Range,
  Compass,
  Sweep,
  DetailTimeout,
  ClockTimeout,
  IdleClock,
};

DisplayAdjustRow s_display_focus = DisplayAdjustRow::Brightness;

// Page 3/3 hosts the radar color plus the audio (beep) options, keeping the
// busy Display page from overflowing the round panel.
enum class ColorsAdjustRow : uint8_t {
  Color,
  BeepOn,
  BeepTone,
};

ColorsAdjustRow s_colors_focus = ColorsAdjustRow::Color;

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

  const size_t raw_len = strlen(text);
  for (size_t n = raw_len; n > 0; --n) {
    snprintf(text, len, "%.*s…", static_cast<int>(n), text);
    if (tft.textWidth(text) <= max_width_px) {
      return;
    }
  }
  strncpy(text, "…", len);
  text[len - 1] = '\0';
}

void drawCenterLine(const char* text, int* y, UiTextStyle style, uint16_t fg,
                    uint16_t bg) {
  displayFontApply(tft, style);
  const int h = displayFontHeight(tft, style);
  const int max_half_w = circleHalfWidthAtRow(*y, h);
  const int max_w = max_half_w * 2;

  char line[64];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  fitLineToWidth(line, sizeof(line), style, max_w);

  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(line, kCenterX, *y);
  *y += h + kLineGap;
}

struct InfoLine {
  const char* text;
  UiTextStyle style;
  uint16_t color;
};

int measureBlockHeight(const InfoLine* lines, size_t count) {
  int total = 0;
  for (size_t i = 0; i < count; ++i) {
    total += displayFontHeight(tft, lines[i].style);
    if (i + 1 < count) {
      total += kLineGap;
    }
  }
  return total;
}

void formatRouteApiLine(const char* name, bool enabled, bool has_key, bool can_use,
                        char* out, size_t out_len) {
  if (!enabled) {
    snprintf(out, out_len, "%s: off", name);
  } else if (!has_key) {
    snprintf(out, out_len, "%s: no key", name);
  } else if (!can_use) {
    snprintf(out, out_len, "%s: limit", name);
  } else {
    snprintf(out, out_len, "%s: active", name);
  }
}

void buildApiStatusStrings(char* airlabs_line, size_t airlabs_len, char* fa_line,
                           size_t fa_len, char* fr24_line, size_t fr24_len) {
  services::apikeys::load();
  formatRouteApiLine("AirLabs", services::apikeys::useAirLabs(),
                     services::apikeys::hasAirLabs(), services::apikeys::canUseAirLabs(),
                     airlabs_line, airlabs_len);
  formatRouteApiLine("FlightAware", services::apikeys::useFlightAware(),
                     services::apikeys::hasFlightAware(),
                     services::apikeys::canUseFlightAware(), fa_line, fa_len);
  formatRouteApiLine("FR24", services::apikeys::useFr24(), services::apikeys::hasFr24(),
                     services::apikeys::canUseFr24(), fr24_line, fr24_len);
}

void buildMainStrings(char* ip_line, size_t ip_len, char* wifi_line, size_t wifi_len,
                      char* lat_line, size_t lat_len, char* lon_line, size_t lon_len,
                      char* alt_line, size_t alt_len, char* web_line, size_t web_len) {
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(ip_line, ip_len, "IP: %s", WiFi.localIP().toString().c_str());
    snprintf(wifi_line, wifi_len, "Wi-Fi: %s", WiFi.SSID().c_str());
  } else {
    snprintf(ip_line, ip_len, "IP: Not connected");
    snprintf(wifi_line, wifi_len, "Wi-Fi: —");
  }

  snprintf(lat_line, lat_len, "Lat: %.5f", services::map_center::latitude());
  snprintf(lon_line, lon_len, "Lon: %.5f", services::map_center::longitude());

  const int min_ft = services::adsb::altitudeFloorFt();
  if (min_ft > 0) {
    snprintf(alt_line, alt_len, "Min alt: %d ft", min_ft);
  } else {
    snprintf(alt_line, alt_len, "Min alt: off");
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(web_line, web_len, "Web: %s.local", config::kPortalHostname);
  } else {
    snprintf(web_line, web_len, "Web: —");
  }
}

void buildDisplayStrings(char* bright_line, size_t bright_len, char* units_line,
                         size_t units_len, char* range_line, size_t range_len,
                         char* compass_line, size_t compass_len, char* sweep_line,
                         size_t sweep_len, char* detail_line, size_t detail_len,
                         char* clock_line, size_t clock_len, char* idle_line,
                         size_t idle_len) {
  snprintf(bright_line, bright_len, "Brightness: %u%%",
           static_cast<unsigned>(hardware::displayBrightnessPercent()));
  snprintf(units_line, units_len, "Units: %s", ui::radar::distanceUnitLabel());
  char range_tag[12];
  ui::radar::formatActiveScaleTag(range_tag, sizeof(range_tag));
  snprintf(range_line, range_len, "Range: %s", range_tag);
  snprintf(compass_line, compass_len, "Compass Rose: %s",
           ui::radar::showCompassRose() ? "on" : "off");
  snprintf(sweep_line, sweep_len, "Radar Sweep: %s",
           ui::displayPrefsSweepLineEnabled() ? "on" : "off");
  snprintf(detail_line, detail_len, "Flight Detail: %s",
           ui::displayPrefsFlightDetailTimeoutLabel());
  snprintf(clock_line, clock_len, "Clock/Forecast: %s",
           ui::displayPrefsClockWeatherTimeoutLabel());
  snprintf(idle_line, idle_len, "Idle Clock: %s",
           ui::displayPrefsAutoIdleClockEnabled() ? "on" : "off");
}

void drawMainPage(uint16_t bg, uint16_t fg, uint16_t label_fg, uint16_t hint_fg) {
  char ip_line[40];
  char wifi_line[40];
  char lat_line[24];
  char lon_line[24];
  char alt_line[24];
  char web_line[36];
  char airlabs_line[24];
  char fa_line[28];
  char fr24_line[20];

  buildMainStrings(ip_line, sizeof(ip_line), wifi_line, sizeof(wifi_line), lat_line,
                   sizeof(lat_line), lon_line, sizeof(lon_line), alt_line,
                   sizeof(alt_line), web_line, sizeof(web_line));
  buildApiStatusStrings(airlabs_line, sizeof(airlabs_line), fa_line, sizeof(fa_line),
                        fr24_line, sizeof(fr24_line));

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine main_lines[] = {
      {ip_line, displayFontBody(), fg},
      {wifi_line, displayFontBody(), fg},
      {lat_line, displayFontDetail(), label_fg},
      {lon_line, displayFontDetail(), label_fg},
      {alt_line, displayFontDetail(), label_fg},
      {web_line, displayFontDetail(), hint_fg},
  };
  const InfoLine api_lines[] = {
      {"Route APIs", displayFontDetail(), hint_fg},
      {airlabs_line, displayFontDetail(), label_fg},
      {fa_line, displayFontDetail(), label_fg},
      {fr24_line, displayFontDetail(), label_fg},
  };
  const InfoLine hint_lines[] = {
      {"Swipe left — Display", displayFontDetail(), hint_fg},
      {"Swipe right — Radar", displayFontDetail(), hint_fg},
  };
  const int main_h = measureBlockHeight(main_lines, sizeof(main_lines) / sizeof(main_lines[0]));
  const int api_h = measureBlockHeight(api_lines, sizeof(api_lines) / sizeof(api_lines[0]));
  const int hints_h = measureBlockHeight(hint_lines, sizeof(hint_lines) / sizeof(hint_lines[0]));
  const int block_h = title_h + kTitleGap + main_h + kSectionGap + api_h + kHintsTopGap +
                      hints_h + kFooterGap;

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Settings 1/3", kCenterX, y);
  y += title_h + kTitleGap;

  for (const InfoLine& line : main_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  y += kSectionGap;

  auto api_color = [&](bool enabled, bool has_key, bool can_use) -> uint16_t {
    return (enabled && has_key && can_use) ? fg : label_fg;
  };

  drawCenterLine("Route APIs", &y, displayFontDetail(), hint_fg, bg);
  drawCenterLine(airlabs_line, &y, displayFontDetail(),
                 api_color(services::apikeys::useAirLabs(), services::apikeys::hasAirLabs(),
                           services::apikeys::canUseAirLabs()),
                 bg);
  drawCenterLine(fa_line, &y, displayFontDetail(),
                 api_color(services::apikeys::useFlightAware(),
                           services::apikeys::hasFlightAware(),
                           services::apikeys::canUseFlightAware()),
                 bg);
  drawCenterLine(fr24_line, &y, displayFontDetail(),
                 api_color(services::apikeys::useFr24(), services::apikeys::hasFr24(),
                           services::apikeys::canUseFr24()),
                 bg);

  y += kHintsTopGap;

  for (const InfoLine& line : hint_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }
}

uint16_t settingsActiveFg() {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  ui::radar::accentHighlightRgb(&r, &g, &b);
  return tft.color565(r, g, b);
}

void drawDisplayPage(uint16_t bg, uint16_t fg, uint16_t label_fg, uint16_t hint_fg) {
  char bright_line[32];
  char units_line[24];
  char range_line[24];
  char compass_line[28];
  char sweep_line[24];
  char detail_line[28];
  char clock_line[28];
  char idle_line[24];
  buildDisplayStrings(bright_line, sizeof(bright_line), units_line, sizeof(units_line),
                      range_line, sizeof(range_line), compass_line, sizeof(compass_line),
                      sweep_line, sizeof(sweep_line), detail_line, sizeof(detail_line),
                      clock_line, sizeof(clock_line), idle_line, sizeof(idle_line));

  const uint16_t active_fg = settingsActiveFg();
  const uint16_t bright_fg =
      (s_display_focus == DisplayAdjustRow::Brightness) ? active_fg : label_fg;
  const uint16_t units_fg =
      (s_display_focus == DisplayAdjustRow::Units) ? active_fg : label_fg;
  const uint16_t range_fg =
      (s_display_focus == DisplayAdjustRow::Range) ? active_fg : label_fg;
  const uint16_t compass_fg =
      (s_display_focus == DisplayAdjustRow::Compass) ? active_fg : label_fg;
  const uint16_t sweep_fg =
      (s_display_focus == DisplayAdjustRow::Sweep) ? active_fg : label_fg;
  const uint16_t detail_fg =
      (s_display_focus == DisplayAdjustRow::DetailTimeout) ? active_fg : label_fg;
  const uint16_t clock_fg =
      (s_display_focus == DisplayAdjustRow::ClockTimeout) ? active_fg : label_fg;
  const uint16_t idle_fg =
      (s_display_focus == DisplayAdjustRow::IdleClock) ? active_fg : label_fg;

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine option_lines[] = {
      {bright_line, displayFontBody(), bright_fg},
      {units_line, displayFontBody(), units_fg},
      {range_line, displayFontBody(), range_fg},
      {compass_line, displayFontBody(), compass_fg},
      {sweep_line, displayFontBody(), sweep_fg},
      {detail_line, displayFontBody(), detail_fg},
      {clock_line, displayFontBody(), clock_fg},
      {idle_line, displayFontBody(), idle_fg},
  };
  const InfoLine hint_lines[] = {
      {"Knob press: change item", displayFontDetail(), hint_fg},
      {"Turn knob: change value", displayFontDetail(), hint_fg},
      {"Swipe left — Colors", displayFontDetail(), hint_fg},
      {"Swipe right — Settings", displayFontDetail(), hint_fg},
  };
  const int options_h = measureBlockHeight(option_lines, sizeof(option_lines) / sizeof(option_lines[0]));
  const int hints_h = measureBlockHeight(hint_lines, sizeof(hint_lines) / sizeof(hint_lines[0]));
  const int block_h = title_h + kTitleGap + options_h + kHintsTopGap + hints_h + kFooterGap;

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Settings 2/3", kCenterX, y);
  y += title_h + kTitleGap;

  for (const InfoLine& line : option_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  y += kHintsTopGap;

  for (const InfoLine& line : hint_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }
}

void drawColorsPage(uint16_t bg, uint16_t fg, uint16_t label_fg, uint16_t hint_fg) {
  char color_line[28];
  char beep_line[24];
  char beep_tone_line[28];
  snprintf(color_line, sizeof(color_line), "Radar color: %s",
           ui::radar::accentColorName());
  snprintf(beep_line, sizeof(beep_line), "UI Beep: %s",
           hardware::buzzerEnabled() ? "on" : "off");
  snprintf(beep_tone_line, sizeof(beep_tone_line), "Beep Tone: %c",
           hardware::buzzerToneLetter());

  const uint16_t active_fg = settingsActiveFg();
  const uint16_t color_fg =
      (s_colors_focus == ColorsAdjustRow::Color) ? active_fg : label_fg;
  const uint16_t beep_fg =
      (s_colors_focus == ColorsAdjustRow::BeepOn) ? active_fg : label_fg;
  const uint16_t beep_tone_fg =
      (s_colors_focus == ColorsAdjustRow::BeepTone) ? active_fg : label_fg;

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine option_lines[] = {
      {color_line, displayFontBody(), color_fg},
      {beep_line, displayFontBody(), beep_fg},
      {beep_tone_line, displayFontBody(), beep_tone_fg},
  };
  const InfoLine hint_lines[] = {
      {"Knob press: change item", displayFontDetail(), hint_fg},
      {"Turn knob: change value", displayFontDetail(), hint_fg},
      {"Swipe right — Display", displayFontDetail(), hint_fg},
  };
  const int options_h = measureBlockHeight(option_lines, sizeof(option_lines) / sizeof(option_lines[0]));
  const int hints_h = measureBlockHeight(hint_lines, sizeof(hint_lines) / sizeof(hint_lines[0]));
  const int block_h = title_h + kTitleGap + options_h + kHintsTopGap + hints_h + kFooterGap;

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Settings 3/3", kCenterX, y);
  y += title_h + kTitleGap;

  for (const InfoLine& line : option_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  y += kHintsTopGap;

  for (const InfoLine& line : hint_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }
}

}  // namespace

namespace ui {

void infoScreenResetToMain() { s_page = ui::InfoSettingsPage::Main; }

void infoScreenResetDisplayFocus() {
  s_display_focus = DisplayAdjustRow::Brightness;
}

void infoScreenCycleDisplayFocus() {
  switch (s_display_focus) {
    case DisplayAdjustRow::Brightness:
      s_display_focus = DisplayAdjustRow::Units;
      break;
    case DisplayAdjustRow::Units:
      s_display_focus = DisplayAdjustRow::Range;
      break;
    case DisplayAdjustRow::Range:
      s_display_focus = DisplayAdjustRow::Compass;
      break;
    case DisplayAdjustRow::Compass:
      s_display_focus = DisplayAdjustRow::Sweep;
      break;
    case DisplayAdjustRow::Sweep:
      s_display_focus = DisplayAdjustRow::DetailTimeout;
      break;
    case DisplayAdjustRow::DetailTimeout:
      s_display_focus = DisplayAdjustRow::ClockTimeout;
      break;
    case DisplayAdjustRow::ClockTimeout:
      s_display_focus = DisplayAdjustRow::IdleClock;
      break;
    default:
      s_display_focus = DisplayAdjustRow::Brightness;
      break;
  }
}

void infoScreenResetColorsFocus() { s_colors_focus = ColorsAdjustRow::Color; }

void infoScreenCycleColorsFocus() {
  switch (s_colors_focus) {
    case ColorsAdjustRow::Color:
      s_colors_focus = ColorsAdjustRow::BeepOn;
      break;
    case ColorsAdjustRow::BeepOn:
      s_colors_focus = ColorsAdjustRow::BeepTone;
      break;
    default:
      s_colors_focus = ColorsAdjustRow::Color;
      break;
  }
}

InfoSettingsPage infoScreenPage() { return s_page; }

void infoScreenSetPage(InfoSettingsPage page) { s_page = page; }

void infoScreenDraw() {
  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  const uint16_t hint_fg = tft.color565(120, 140, 160);

  tft.fillScreen(bg);

  if (s_page == ui::InfoSettingsPage::Main) {
    drawMainPage(bg, fg, label_fg, hint_fg);
  } else if (s_page == ui::InfoSettingsPage::Display) {
    drawDisplayPage(bg, fg, label_fg, hint_fg);
  } else {
    drawColorsPage(bg, fg, label_fg, hint_fg);
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

void infoScreenHandleKnob(int8_t delta) {
  if (delta == 0) {
    return;
  }

  if (s_page == ui::InfoSettingsPage::Colors) {
    switch (s_colors_focus) {
      case ColorsAdjustRow::Color:
        ui::radar::accentStep(delta);
        break;
      case ColorsAdjustRow::BeepOn:
        hardware::buzzerSetEnabled(!hardware::buzzerEnabled());
        break;
      case ColorsAdjustRow::BeepTone:
        hardware::buzzerToneStep(delta);
        break;
    }
    infoScreenDraw();
    return;
  }

  if (s_page != ui::InfoSettingsPage::Display) {
    return;
  }

  switch (s_display_focus) {
    case DisplayAdjustRow::Brightness:
      hardware::displayBrightnessStep(delta);
      break;
    case DisplayAdjustRow::Units:
      ui::radar::cycleDistanceUnits();
      break;
    case DisplayAdjustRow::Range:
      ui::radar::scaleStep(delta);
      break;
    case DisplayAdjustRow::Compass:
      ui::radar::toggleCompassRose();
      break;
    case DisplayAdjustRow::Sweep:
      ui::displayPrefsToggleSweepLine();
      break;
    case DisplayAdjustRow::DetailTimeout:
      ui::displayPrefsFlightDetailTimeoutStep(delta);
      break;
    case DisplayAdjustRow::ClockTimeout:
      ui::displayPrefsClockWeatherTimeoutStep(delta);
      break;
    case DisplayAdjustRow::IdleClock:
      ui::displayPrefsToggleAutoIdleClock();
      break;
  }
  infoScreenDraw();
}

}  // namespace ui
