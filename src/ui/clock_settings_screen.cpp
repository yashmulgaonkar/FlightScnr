#include "ui/clock_settings_screen.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock_time.h"
#include "ui/radar_accent.h"
#include "ui/radar_theme.h"

namespace ui {
namespace {

constexpr int kBezelInsetPx = 10;
constexpr int kTextPadPx = 6;
constexpr int kTitleGap = 6;
constexpr int kLineGap = 4;
constexpr int kFooterGap = 8;
/** Space between editable options and help hints. */
constexpr int kHintsTopGap = 22;

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

enum class ClockSettingsRow : uint8_t { Timezone, Format, DateFormat };

ClockSettingsRow s_focus = ClockSettingsRow::Timezone;

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

  char line[56];
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

void buildStrings(char* tz_line, size_t tz_len, char* fmt_line, size_t fmt_len,
                  char* date_line, size_t date_len) {
  char tz_label[16];
  services::clock::formatTimezoneLabel(tz_label, sizeof(tz_label));
  snprintf(tz_line, tz_len, "Timezone: %s", tz_label);
  snprintf(fmt_line, fmt_len, "Time format: %s",
           services::clock::use24Hour() ? "24 hour" : "12 hour");
  snprintf(date_line, date_len, "Date format: %s",
           services::clock::useNumericDate() ? "numeric" : "text");
}

}  // namespace

void clockSettingsResetFocus() { s_focus = ClockSettingsRow::Timezone; }

void clockSettingsCycleFocus() {
  switch (s_focus) {
    case ClockSettingsRow::Timezone:
      s_focus = ClockSettingsRow::Format;
      break;
    case ClockSettingsRow::Format:
      s_focus = ClockSettingsRow::DateFormat;
      break;
    case ClockSettingsRow::DateFormat:
      s_focus = ClockSettingsRow::Timezone;
      break;
  }
}

void clockSettingsScreenDraw() {
  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  const uint16_t hint_fg = tft.color565(120, 140, 160);
  uint8_t accent_r = 0;
  uint8_t accent_g = 0;
  uint8_t accent_b = 0;
  radar::accentHighlightRgb(&accent_r, &accent_g, &accent_b);
  const uint16_t active_fg = tft.color565(accent_r, accent_g, accent_b);

  char tz_line[32];
  char fmt_line[28];
  char date_line[28];
  buildStrings(tz_line, sizeof(tz_line), fmt_line, sizeof(fmt_line), date_line,
               sizeof(date_line));

  const uint16_t tz_fg =
      (s_focus == ClockSettingsRow::Timezone) ? active_fg : label_fg;
  const uint16_t fmt_fg =
      (s_focus == ClockSettingsRow::Format) ? active_fg : label_fg;
  const uint16_t date_fg =
      (s_focus == ClockSettingsRow::DateFormat) ? active_fg : label_fg;

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine option_lines[] = {
      {tz_line, displayFontBody(), tz_fg},
      {fmt_line, displayFontBody(), fmt_fg},
      {date_line, displayFontBody(), date_fg},
  };
  const InfoLine hint_lines[] = {
      {"Knob press: change item", displayFontDetail(), hint_fg},
      {"Turn knob: adjust value", displayFontDetail(), hint_fg},
      {"Swipe right — Clock", displayFontDetail(), hint_fg},
  };
  const int options_h = measureBlockHeight(option_lines, sizeof(option_lines) / sizeof(option_lines[0]));
  const int hints_h = measureBlockHeight(hint_lines, sizeof(hint_lines) / sizeof(hint_lines[0]));
  const int block_h = title_h + kTitleGap + options_h + kHintsTopGap + hints_h + kFooterGap;

  tft.fillScreen(bg);

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Clock settings", kCenterX, y);
  y += title_h + kTitleGap;

  for (const InfoLine& line : option_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  y += kHintsTopGap;

  for (const InfoLine& line : hint_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

void clockSettingsHandleKnob(int8_t delta) {
  if (delta == 0) {
    return;
  }

  switch (s_focus) {
    case ClockSettingsRow::Timezone:
      services::clock::stepTimezoneHours(delta);
      break;
    case ClockSettingsRow::Format:
      services::clock::toggleHourFormat();
      break;
    case ClockSettingsRow::DateFormat:
      services::clock::toggleDateFormat();
      break;
  }
  clockSettingsScreenDraw();
}

}  // namespace ui
