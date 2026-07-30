#include "ui/disclaimer_screen.h"

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"

namespace ui {
namespace {

constexpr int kLineGap = 2;
constexpr int kTitleBodyGap = 8;
constexpr int kTopSafePx = 40;

constexpr int kBtnW = 120;
constexpr int kBtnH = 36;
constexpr int kBtnBottomInset = 58;
constexpr int kBodyBtnGap = 12;

const int kCenterX = config::kDisplayWidth / 2;

int s_btn_x = 0;
int s_btn_y = 0;
int s_btn_w = kBtnW;
int s_btn_h = kBtnH;

/** Snap rect to even coords so 2px borders survive pixelAlign2 panels. */
int alignEven(int v) { return v & ~1; }

void updateButtonRect() {
  s_btn_w = alignEven(kBtnW);
  s_btn_h = alignEven(kBtnH);
  s_btn_x = alignEven(kCenterX - s_btn_w / 2);
  s_btn_y = alignEven(config::kDisplayHeight - kBtnBottomInset - s_btn_h);
}

bool canDrawLine(int y, int h, int limit_y) { return y >= 0 && (y + h) <= limit_y; }

void drawCenterLineClamped(const char* text, int* y, UiTextStyle style, uint16_t fg,
                           uint16_t bg, int limit_y) {
  displayFontApply(tft, style);
  const int h = displayFontHeight(tft, style);
  if (!canDrawLine(*y, h, limit_y)) {
    return;
  }
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(text, kCenterX, *y);
  *y += h + kLineGap;
}

}  // namespace

bool disclaimerScreenHitAccept(int16_t x, int16_t y) {
  constexpr int kPad = 10;
  return x >= (s_btn_x - kPad) && x < (s_btn_x + s_btn_w + kPad) &&
         y >= (s_btn_y - kPad) && y < (s_btn_y + s_btn_h + kPad);
}

void disclaimerScreenDraw() {
  updateButtonRect();

  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t body_fg = tft.color565(190, 200, 210);
  const uint16_t btn_fill = tft.color565(42, 42, 42);
  const uint16_t btn_edge = tft.color565(200, 200, 200);

  tft.fillScreen(bg);

  // Same wording as before; broken into short fixed lines for the round face.
  const char* title_lines[] = {
      "NOT FOR",
      "SAFETY CRITICAL",
      "USE",
  };
  const char* body_lines[] = {
      "FlightScnr is not certified or",
      "cleared for aviation use.",
      "",
      "Do not rely on it for flight",
      "situational awareness or",
      "safety decisions.",
      "",
      "It heavily relies on external",
      "APIs. Uptime is not guaranteed.",
  };

  const UiTextStyle title_style = displayFontBody();
  const UiTextStyle body_style = displayFontScale();
  const int title_h = displayFontHeight(tft, title_style);
  const int body_h = displayFontHeight(tft, body_style);

  const int text_limit_y = s_btn_y - kBodyBtnGap;

  int block_h = 0;
  for (const char* line : title_lines) {
    (void)line;
    block_h += title_h + kLineGap;
  }
  block_h += kTitleBodyGap;
  for (const char* line : body_lines) {
    if (line[0] == '\0') {
      block_h += body_h / 2;
    } else {
      block_h += body_h + kLineGap;
    }
  }

  int y = kTopSafePx;
  if (y + block_h < text_limit_y) {
    y += (text_limit_y - y - block_h) / 4;
  }

  for (const char* line : title_lines) {
    drawCenterLineClamped(line, &y, title_style, fg, bg, text_limit_y);
  }
  y += kTitleBodyGap - kLineGap;

  for (const char* line : body_lines) {
    if (line[0] == '\0') {
      y += body_h / 2;
      continue;
    }
    drawCenterLineClamped(line, &y, body_style, body_fg, bg, text_limit_y);
  }

  // Draw Accept last so it never sits under text; even-aligned 2px border.
  tft.fillRect(s_btn_x, s_btn_y, s_btn_w, s_btn_h, btn_fill);
  tft.fillRect(s_btn_x, s_btn_y, s_btn_w, 2, btn_edge);
  tft.fillRect(s_btn_x, s_btn_y + s_btn_h - 2, s_btn_w, 2, btn_edge);
  tft.fillRect(s_btn_x, s_btn_y, 2, s_btn_h, btn_edge);
  tft.fillRect(s_btn_x + s_btn_w - 2, s_btn_y, 2, s_btn_h, btn_edge);

  displayFontApply(tft, displayFontBody());
  tft.setTextDatum(TextDatum::MiddleCenter);
  tft.setTextColor(fg, btn_fill);
  tft.drawString("Accept", s_btn_x + s_btn_w / 2, s_btn_y + s_btn_h / 2);

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

}  // namespace ui
