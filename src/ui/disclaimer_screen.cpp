#include "ui/disclaimer_screen.h"

#include <cstdio>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"

namespace ui {
namespace {

constexpr int kLineGap = 2;
constexpr int kTitleBodyGap = 8;
constexpr int kTopSafePx = 36;

constexpr int kBtnW = 120;
constexpr int kBtnH = 36;
/** Keep inside the round bezel; room above for checkbox + countdown. */
constexpr int kBtnBottomInset = 24;
constexpr int kBodyBtnGap = 10;
constexpr int kCheckSize = 18;
constexpr int kCheckGap = 8;
constexpr int kCheckRowGap = 6;
constexpr int kHitPad = 10;

const int kCenterX = config::kDisplayWidth / 2;
const int kCountdownY = config::kDisplayHeight - 64;
constexpr int kCountdownRegionX = 40;
const int kCountdownRegionW = config::kDisplayWidth - 2 * kCountdownRegionX;
constexpr int kCountdownRegionH = 30;

int s_btn_x = 0;
int s_btn_y = 0;
int s_btn_w = kBtnW;
int s_btn_h = kBtnH;

int s_check_box_x = 0;
int s_check_box_y = 0;
int s_check_hit_x = 0;
int s_check_hit_y = 0;
int s_check_hit_w = 0;
int s_check_hit_h = 0;

bool s_remember_checked = false;
bool s_countdown_mode = false;

/** Snap rect to even coords so 2px borders survive pixelAlign2 panels. */
int alignEven(int v) { return v & ~1; }

void updateControlRects() {
  s_btn_w = alignEven(kBtnW);
  s_btn_h = alignEven(kBtnH);
  s_btn_x = alignEven(kCenterX - s_btn_w / 2);
  s_btn_y = alignEven(config::kDisplayHeight - kBtnBottomInset - s_btn_h);

  displayFontApply(tft, displayFontScale());
  const int label_h = displayFontHeight(tft, displayFontScale());
  const char* label = "Don't show again";
  const int label_w = tft.textWidth(label);
  const int row_h = label_h > kCheckSize ? label_h : kCheckSize;
  const int row_w = kCheckSize + kCheckGap + label_w;
  const int row_x = alignEven(kCenterX - row_w / 2);
  const int row_y = alignEven(s_btn_y - kCheckRowGap - row_h);

  s_check_box_x = row_x;
  s_check_box_y = alignEven(row_y + (row_h - kCheckSize) / 2);
  s_check_hit_x = row_x - kHitPad;
  s_check_hit_y = row_y - kHitPad;
  s_check_hit_w = row_w + 2 * kHitPad;
  s_check_hit_h = row_h + 2 * kHitPad;
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

void drawCountdown(int countdown_sec, uint16_t fg, uint16_t bg) {
  tft.fillRect(kCountdownRegionX, kCountdownY, kCountdownRegionW,
               kCountdownRegionH, bg);
  char countdown[28];
  snprintf(countdown, sizeof(countdown), "Continuing in %d…", countdown_sec);
  displayFontApply(tft, displayFontDetail());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(countdown, kCenterX, kCountdownY);
}

}  // namespace

void disclaimerScreenReset(bool remember_checked) {
  s_remember_checked = remember_checked;
  s_countdown_mode = false;
}

bool disclaimerScreenRememberChecked() { return s_remember_checked; }

void disclaimerScreenSetRememberChecked(bool checked) { s_remember_checked = checked; }

void disclaimerScreenToggleRemember() { s_remember_checked = !s_remember_checked; }

bool disclaimerScreenHitAccept(int16_t x, int16_t y) {
  if (s_countdown_mode) {
    return false;
  }
  return x >= (s_btn_x - kHitPad) && x < (s_btn_x + s_btn_w + kHitPad) &&
         y >= (s_btn_y - kHitPad) && y < (s_btn_y + s_btn_h + kHitPad);
}

bool disclaimerScreenHitRemember(int16_t x, int16_t y) {
  if (s_countdown_mode) {
    return false;
  }
  return x >= s_check_hit_x && x < (s_check_hit_x + s_check_hit_w) &&
         y >= s_check_hit_y && y < (s_check_hit_y + s_check_hit_h);
}

void disclaimerScreenDraw(int countdown_sec) {
  updateControlRects();
  s_countdown_mode = countdown_sec >= 0;

  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t body_fg = tft.color565(190, 200, 210);
  const uint16_t dim_fg = tft.color565(150, 165, 180);
  const uint16_t btn_fill = tft.color565(42, 42, 42);
  const uint16_t btn_edge = tft.color565(200, 200, 200);
  const uint16_t check_fill = s_remember_checked ? tft.color565(60, 140, 80) : btn_fill;

  tft.fillScreen(bg);

  // Same wording as before; broken into short fixed lines for the round face.
  // Bump services::disclaimer::kCurrentVersion when this text changes.
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

  const int text_limit_y =
      s_countdown_mode ? (kCountdownY - kBodyBtnGap)
                       : (s_check_hit_y + kHitPad - kBodyBtnGap);

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

  if (s_countdown_mode) {
    // Remembered boots are informational only: no Accept button or checkbox.
    drawCountdown(countdown_sec, dim_fg, bg);
    tft.setTextDatum(TextDatum::TopLeft);
    tft.endOffscreen();
    return;
  }

  // Checkbox row.
  tft.fillRect(s_check_box_x, s_check_box_y, kCheckSize, kCheckSize, check_fill);
  tft.fillRect(s_check_box_x, s_check_box_y, kCheckSize, 2, btn_edge);
  tft.fillRect(s_check_box_x, s_check_box_y + kCheckSize - 2, kCheckSize, 2, btn_edge);
  tft.fillRect(s_check_box_x, s_check_box_y, 2, kCheckSize, btn_edge);
  tft.fillRect(s_check_box_x + kCheckSize - 2, s_check_box_y, 2, kCheckSize, btn_edge);
  if (s_remember_checked) {
    constexpr int kInner = 8;
    const int ix = s_check_box_x + (kCheckSize - kInner) / 2;
    const int iy = s_check_box_y + (kCheckSize - kInner) / 2;
    tft.fillRect(ix, iy, kInner, kInner, fg);
  }

  displayFontApply(tft, displayFontScale());
  tft.setTextDatum(TextDatum::MiddleLeft);
  tft.setTextColor(body_fg, bg);
  tft.drawString("Don't show again", s_check_box_x + kCheckSize + kCheckGap,
                 s_check_box_y + kCheckSize / 2);

  // Accept button last so it never sits under text; even-aligned 2px border.
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

void disclaimerScreenUpdateCountdown(int countdown_sec) {
  if (!s_countdown_mode || countdown_sec < 0) {
    return;
  }
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t dim_fg = tft.color565(150, 165, 180);
  const bool offscreen = tft.beginOffscreen();
  drawCountdown(countdown_sec, dim_fg, bg);
  tft.setTextDatum(TextDatum::TopLeft);
  if (offscreen) {
    tft.endOffscreenRegion(kCountdownRegionX, kCountdownY,
                           kCountdownRegionW, kCountdownRegionH);
  }
}

}  // namespace ui
