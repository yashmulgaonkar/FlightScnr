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
/** Space between main content and help hints. */
constexpr int kHintsTopGap = 22;

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

ui::InfoSettingsPage s_page = ui::InfoSettingsPage::Main;

enum class DisplayAdjustRow : uint8_t {
  Brightness,
  Units,
  Compass,
  Sweep,
  DetailTimeout,
  BeepOn,
  BeepTone,
};

DisplayAdjustRow s_display_focus = DisplayAdjustRow::Brightness;

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
                         size_t units_len, char* compass_line, size_t compass_len,
                         char* sweep_line, size_t sweep_len, char* detail_line,
                         size_t detail_len, char* beep_line, size_t beep_len,
                         char* beep_tone_line, size_t beep_tone_len) {
  snprintf(bright_line, bright_len, "Brightness: %u%%",
           static_cast<unsigned>(hardware::displayBrightnessPercent()));
  snprintf(units_line, units_len, "Units: %s",
           ui::radar::distanceInMiles() ? "miles" : "km");
  snprintf(compass_line, compass_len, "Compass Rose: %s",
           ui::radar::showCompassRose() ? "on" : "off");
  snprintf(sweep_line, sweep_len, "Radar Sweep: %s",
           ui::displayPrefsSweepLineEnabled() ? "on" : "off");
  snprintf(detail_line, detail_len, "Flight Detail: %s",
           ui::displayPrefsFlightDetailTimeoutLabel());
  snprintf(beep_line, beep_len, "UI Beep: %s",
           hardware::buzzerEnabled() ? "on" : "off");
  snprintf(beep_tone_line, beep_tone_len, "Beep Tone: %c",
           hardware::buzzerToneLetter());
}

void drawMainPage(uint16_t bg, uint16_t fg, uint16_t label_fg, uint16_t hint_fg) {
  char ip_line[40];
  char wifi_line[40];
  char lat_line[24];
  char lon_line[24];
  char alt_line[24];
  char web_line[36];

  buildMainStrings(ip_line, sizeof(ip_line), wifi_line, sizeof(wifi_line), lat_line,
                   sizeof(lat_line), lon_line, sizeof(lon_line), alt_line,
                   sizeof(alt_line), web_line, sizeof(web_line));

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine main_lines[] = {
      {ip_line, displayFontBody(), fg},
      {wifi_line, displayFontBody(), fg},
      {lat_line, displayFontDetail(), label_fg},
      {lon_line, displayFontDetail(), label_fg},
      {alt_line, displayFontDetail(), label_fg},
      {web_line, displayFontDetail(), hint_fg},
  };
  const InfoLine hint_lines[] = {
      {"Swipe left — Display", displayFontDetail(), hint_fg},
      {"Swipe right — Radar", displayFontDetail(), hint_fg},
  };
  const int main_h = measureBlockHeight(main_lines, sizeof(main_lines) / sizeof(main_lines[0]));
  const int hints_h = measureBlockHeight(hint_lines, sizeof(hint_lines) / sizeof(hint_lines[0]));
  const int block_h = title_h + kTitleGap + main_h + kHintsTopGap + hints_h + kFooterGap;

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
  char compass_line[28];
  char sweep_line[24];
  char detail_line[28];
  char beep_line[24];
  char beep_tone_line[28];
  buildDisplayStrings(bright_line, sizeof(bright_line), units_line, sizeof(units_line),
                      compass_line, sizeof(compass_line), sweep_line, sizeof(sweep_line),
                      detail_line, sizeof(detail_line), beep_line, sizeof(beep_line),
                      beep_tone_line, sizeof(beep_tone_line));

  const uint16_t active_fg = settingsActiveFg();
  const uint16_t bright_fg =
      (s_display_focus == DisplayAdjustRow::Brightness) ? active_fg : label_fg;
  const uint16_t units_fg =
      (s_display_focus == DisplayAdjustRow::Units) ? active_fg : label_fg;
  const uint16_t compass_fg =
      (s_display_focus == DisplayAdjustRow::Compass) ? active_fg : label_fg;
  const uint16_t sweep_fg =
      (s_display_focus == DisplayAdjustRow::Sweep) ? active_fg : label_fg;
  const uint16_t detail_fg =
      (s_display_focus == DisplayAdjustRow::DetailTimeout) ? active_fg : label_fg;
  const uint16_t beep_fg =
      (s_display_focus == DisplayAdjustRow::BeepOn) ? active_fg : label_fg;
  const uint16_t beep_tone_fg =
      (s_display_focus == DisplayAdjustRow::BeepTone) ? active_fg : label_fg;

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine option_lines[] = {
      {bright_line, displayFontBody(), bright_fg},
      {units_line, displayFontBody(), units_fg},
      {compass_line, displayFontBody(), compass_fg},
      {sweep_line, displayFontBody(), sweep_fg},
      {detail_line, displayFontBody(), detail_fg},
      {beep_line, displayFontBody(), beep_fg},
      {beep_tone_line, displayFontBody(), beep_tone_fg},
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
  snprintf(color_line, sizeof(color_line), "Radar color: %s",
           ui::radar::accentColorName());

  const uint16_t active_fg = settingsActiveFg();

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine option_lines[] = {
      {color_line, displayFontBody(), active_fg},
  };
  const InfoLine hint_lines[] = {
      {"Turn knob: change color", displayFontDetail(), hint_fg},
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
      s_display_focus = DisplayAdjustRow::Compass;
      break;
    case DisplayAdjustRow::Compass:
      s_display_focus = DisplayAdjustRow::Sweep;
      break;
    case DisplayAdjustRow::Sweep:
      s_display_focus = DisplayAdjustRow::DetailTimeout;
      break;
    case DisplayAdjustRow::DetailTimeout:
      s_display_focus = DisplayAdjustRow::BeepOn;
      break;
    case DisplayAdjustRow::BeepOn:
      s_display_focus = DisplayAdjustRow::BeepTone;
      break;
    default:
      s_display_focus = DisplayAdjustRow::Brightness;
      break;
  }
}

InfoSettingsPage infoScreenPage() { return s_page; }

void infoScreenSetPage(InfoSettingsPage page) { s_page = page; }

void infoScreenDraw() {
  const uint16_t bg = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
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
}

void infoScreenHandleKnob(int8_t delta) {
  if (delta == 0) {
    return;
  }

  if (s_page == ui::InfoSettingsPage::Colors) {
    ui::radar::accentStep(delta);
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
      ui::radar::toggleDistanceUnits();
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
    case DisplayAdjustRow::BeepOn:
      hardware::buzzerSetEnabled(!hardware::buzzerEnabled());
      break;
    case DisplayAdjustRow::BeepTone:
      hardware::buzzerToneStep(delta);
      break;
  }
  infoScreenDraw();
}

}  // namespace ui
