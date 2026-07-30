#include "ui/details_screen.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/ota_github.h"
#include "ui/radar_theme.h"

namespace ui {
namespace {

constexpr int kBezelInsetPx = 10;
constexpr int kTextPadPx = 6;
constexpr int kLineGap = 4;
constexpr int kFooterGap = 8;
constexpr int kHintsTopGap = 22;
/** Extra space between firmware version and author lines. */
constexpr int kAuthorTopGap = 20;

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

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

}  // namespace

void detailsScreenDraw(bool boot_splash) {
  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  const uint16_t hint_fg = tft.color565(120, 140, 160);

  char version_line[40];
  snprintf(version_line, sizeof(version_line), "FW Ver: %s", config::kFirmwareVersion);

  const bool show_update = services::ota_github::updateAvailable();
  char update_line[48] = {};
  if (show_update) {
    snprintf(update_line, sizeof(update_line), "Update available");
  }

  const InfoLine version_lines[] = {
      {version_line, displayFontBody(), fg},
  };
  const InfoLine update_lines[] = {
      {update_line, displayFontDetail(), tft.color565(255, 200, 80)},
      {"Use web settings to install", displayFontDetail(), hint_fg},
  };
  const InfoLine author_lines[] = {
      {"FlightScnr by", displayFontBody(), label_fg},
      {"Yash Mulgaonkar", displayFontBody(), label_fg},
  };
  const InfoLine hint_lines[] = {
      {"Swipe down — Radar", displayFontDetail(), hint_fg},
  };

  const int version_h = measureBlockHeight(version_lines, sizeof(version_lines) / sizeof(version_lines[0]));
  const int update_h =
      show_update ? (measureBlockHeight(update_lines, sizeof(update_lines) / sizeof(update_lines[0])) +
                     kLineGap)
                  : 0;
  const int author_h = measureBlockHeight(author_lines, sizeof(author_lines) / sizeof(author_lines[0]));
  const int hints_h = boot_splash ? 0
                                  : measureBlockHeight(hint_lines,
                                                       sizeof(hint_lines) / sizeof(hint_lines[0]));
  const int block_h = version_h + update_h + kAuthorTopGap + author_h +
                      (boot_splash ? 0 : kHintsTopGap + hints_h + kFooterGap);

  tft.fillScreen(bg);

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  for (const InfoLine& line : version_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  if (show_update) {
    y += kLineGap;
    for (const InfoLine& line : update_lines) {
      drawCenterLine(line.text, &y, line.style, line.color, bg);
    }
  }

  y += kAuthorTopGap - kLineGap;

  for (const InfoLine& line : author_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  if (!boot_splash) {
    y += kHintsTopGap;

    for (const InfoLine& line : hint_lines) {
      drawCenterLine(line.text, &y, line.style, line.color, bg);
    }
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

}  // namespace ui
