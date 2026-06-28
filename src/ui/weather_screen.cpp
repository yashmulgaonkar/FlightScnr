#include "ui/weather_screen.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/api_keys.h"
#include "services/clock_time.h"
#include "services/weather.h"
#include "services/weather_icon.h"
#include "ui/radar_accent.h"
#include "ui/radar_theme.h"
#include "ui/temp_label.h"

namespace ui {
namespace {

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

void drawCenteredAt(const char* text, int cx, int y, UiTextStyle style, uint16_t fg,
                    uint16_t bg) {
  displayFontApply(tft, style);
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(text, cx, y);
}

void drawCentered(const char* text, int y, UiTextStyle style, uint16_t fg, uint16_t bg) {
  drawCenteredAt(text, kCenterX, y, style, fg, bg);
}

// Draw "<value>°<unit>" centered at cx (top at top_y).
void drawTempCentered(int cx, int top_y, int value, char unit, UiTextStyle style,
                      uint16_t fg, uint16_t bg) {
  temp_label::drawCentered(cx, top_y, value, unit, style, fg, bg);
}

void weekdayLabel(int64_t date_epoch, int index, char* out, size_t len) {
  if (date_epoch <= 0) {
    snprintf(out, len, "Day %d", index + 1);
    return;
  }
  const time_t utc = static_cast<time_t>(date_epoch);
  struct tm t {};
  localtime_r(&utc, &t);

  // Label the column "Today" only when its date matches the current local date,
  // so the labels stay correct after a midnight rollover even if the cache is
  // briefly stale.
  const time_t now = time(nullptr);
  if (now >= 1600000000) {
    struct tm nt {};
    localtime_r(&now, &nt);
    if (t.tm_year == nt.tm_year && t.tm_yday == nt.tm_yday) {
      strncpy(out, "Today", len - 1);
      out[len - 1] = '\0';
      return;
    }
  } else if (index == 0) {
    strncpy(out, "Today", len - 1);
    out[len - 1] = '\0';
    return;
  }
  strftime(out, len, "%a", &t);
}

void drawForecast(const services::weather::WeatherData& wx, uint16_t fg, uint16_t dim,
                  uint16_t accent, uint16_t bg) {
  const char unit = wx.imperial ? 'F' : 'C';
  const int col_centers[services::weather::kForecastDays] = {kCenterX - 110, kCenterX,
                                                             kCenterX + 110};
  // Smaller icons leave room for the larger hi/lo temperature fonts.
  constexpr int kForecastIconSize = 72;
  constexpr int kLabelY = 104;
  constexpr int kIconY = 128;
  constexpr int kHiY = 214;
  constexpr int kLoY = 256;
  constexpr int kRainY = 292;

  for (int i = 0; i < services::weather::kForecastDays; ++i) {
    const services::weather::DayForecast& d = wx.days[i];
    if (!d.valid) {
      continue;
    }
    const int cx = col_centers[i];
    char label[12];
    weekdayLabel(d.date_epoch, i, label, sizeof(label));
    const bool is_today = strcmp(label, "Today") == 0;
    drawCenteredAt(label, cx, kLabelY, displayFontDetail(), is_today ? accent : fg, bg);
    services::weather_icon::drawIconScaled(tft, services::weather::dayIconCode(i), cx, kIconY,
                                           bg, kForecastIconSize);
    drawTempCentered(cx, kHiY, static_cast<int>(lroundf(d.temp_max)), unit,
                     displayFontTitle(), fg, bg);
    drawTempCentered(cx, kLoY, static_cast<int>(lroundf(d.temp_min)), unit,
                     displayFontBody(), dim, bg);
    if (d.precip_probability >= 0) {
      const int pct = d.precip_probability > 100 ? 100 : d.precip_probability;
      char rain[16];
      snprintf(rain, sizeof(rain), "Rain %d%%", pct);
      drawCenteredAt(rain, cx, kRainY, displayFontDetail(), dim, bg);
    }
  }
}

}  // namespace

void weatherScreenDraw() {
  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t dim = tft.color565(150, 165, 180);
  uint8_t accent_r = 0;
  uint8_t accent_g = 0;
  uint8_t accent_b = 0;
  radar::accentHighlightRgb(&accent_r, &accent_g, &accent_b);
  const uint16_t accent = tft.color565(accent_r, accent_g, accent_b);

  tft.fillScreen(bg);
  drawCentered("Forecast", 48, displayFontClockDate(), fg, bg);

  if (!services::apikeys::useWeather()) {
    drawCentered("Tomorrow.io disabled", kCenterY - 12, displayFontDetail(), dim, bg);
    drawCentered("in web settings", kCenterY + 12, displayFontDetail(), dim, bg);
  } else if (!services::apikeys::hasWeather()) {
    drawCentered("Add a Tomorrow.io key", kCenterY - 12, displayFontDetail(), dim, bg);
    drawCentered("in web settings", kCenterY + 12, displayFontDetail(), dim, bg);
  } else if (!services::weather::hasData()) {
    drawCentered(services::weather::fetchInProgress() ? "Loading weather…"
                                                      : "No weather data",
                 kCenterY - 8, displayFontBody(), dim, bg);
  } else {
    drawForecast(services::weather::data(), fg, dim, accent, bg);
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

}  // namespace ui
