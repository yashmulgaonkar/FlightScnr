#include "ui/boot_screens.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"

namespace {

constexpr int kLineGap = 10;
const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

constexpr int kSonarOuterRadius = 186;
constexpr int kSonarInnerRadius = 118;
constexpr int kSonarRingStepPx = 6;
constexpr uint16_t kSonarGuideColor = 0x0320;

char s_connecting_ssid[33];
char s_ssid_line[33];
char s_portal_ip_alt[32];
constexpr int kConnectingTextMaxWidthPx = 358;
int s_ping_radius = kSonarInnerRadius;
int s_ping_erase_radius = 0;
bool s_connecting_text_drawn = false;
bool s_sonar_guides_drawn = false;

struct TextLine {
  const char* text;
  UiTextStyle style;
};

void drawTextBlock(uint16_t bg, uint16_t fg, const TextLine* lines, size_t count) {
  tft.beginOffscreen();
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(TextDatum::MiddleCenter);

  int total_h = 0;
  for (size_t i = 0; i < count; ++i) {
    total_h += displayFontHeight(tft, lines[i].style);
    if (i + 1 < count) {
      total_h += kLineGap;
    }
  }

  int y = (config::kDisplayHeight - total_h) / 2;
  for (size_t i = 0; i < count; ++i) {
    displayFontApply(tft, lines[i].style);
    const int h = displayFontHeight(tft, lines[i].style);
    tft.drawString(lines[i].text, kCenterX, y + h / 2);
    y += h + kLineGap;
  }
  tft.endOffscreen();
}

void fitSsidLine() {
  strncpy(s_ssid_line, s_connecting_ssid, sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
  displayFontApply(tft, displayFontDetail());
  if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
    return;
  }
  const size_t len = strlen(s_connecting_ssid);
  for (size_t n = len; n > 0; --n) {
    snprintf(s_ssid_line, sizeof(s_ssid_line), "%.*s…", static_cast<int>(n),
             s_connecting_ssid);
    if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
      return;
    }
  }
  strncpy(s_ssid_line, "…", sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
}

void drawConnectingText() {
  // Must use offscreen compose on CO5300 — direct 1-bit glyphs look 2x2 pixelated.
  tft.beginOffscreen();
  tft.fillScreen(config::kColorBlack);

  tft.setTextDatum(TextDatum::MiddleCenter);
  tft.setTextColor(config::kTextOnBlack, config::kColorBlack);

  displayFontApply(tft, displayFontDetail());
  const int detail_h = tft.fontHeight();
  const int total_h = detail_h * 2 + kLineGap;
  const int block_top = (config::kDisplayHeight - total_h) / 2;

  int y = block_top;
  tft.drawString("Connecting to", kCenterX, y + detail_h / 2);
  y += detail_h + kLineGap;
  tft.drawString(s_ssid_line, kCenterX, y + detail_h / 2);
  tft.endOffscreen();

  s_connecting_text_drawn = true;
  // Circles are drawn on the panel after the blit; force guides/ping redo.
  s_sonar_guides_drawn = false;
}

void drawSonarGuides() {
  tft.drawCircle(kCenterX, kCenterY, kSonarOuterRadius, kSonarGuideColor);
  tft.drawCircle(kCenterX, kCenterY, kSonarInnerRadius, kSonarGuideColor);
  s_sonar_guides_drawn = true;
}

void eraseSonarPing() {
  if (s_ping_erase_radius <= 0) {
    return;
  }
  tft.drawCircle(kCenterX, kCenterY, s_ping_erase_radius, config::kColorBlack);
  tft.drawCircle(kCenterX, kCenterY, s_ping_erase_radius - 1, config::kColorBlack);
  s_ping_erase_radius = 0;
}

void drawSonarPing() {
  const int span = kSonarOuterRadius - kSonarInnerRadius;
  const int phase = s_ping_radius - kSonarInnerRadius;
  const int fade = 96 + (159 * phase) / span;
  const uint16_t color = tft.color565(0, static_cast<uint8_t>(fade), 0);
  tft.drawCircle(kCenterX, kCenterY, s_ping_radius, color);
  s_ping_erase_radius = s_ping_radius;
}

void advanceSonarPing() {
  s_ping_radius += kSonarRingStepPx;
  if (s_ping_radius > kSonarOuterRadius) {
    s_ping_radius = kSonarInnerRadius;
  }
}

}  // namespace

void bootScreenConnectingStart(const char* ssid) {
  const char* name = (ssid != nullptr && ssid[0] != '\0') ? ssid : "network";
  strncpy(s_connecting_ssid, name, sizeof(s_connecting_ssid) - 1);
  s_connecting_ssid[sizeof(s_connecting_ssid) - 1] = '\0';
  fitSsidLine();
  s_ping_radius = kSonarInnerRadius;
  s_ping_erase_radius = 0;
  s_connecting_text_drawn = false;
  s_sonar_guides_drawn = false;
  drawConnectingText();
  if (!s_sonar_guides_drawn) {
    drawSonarGuides();
  }
  drawSonarPing();
}

void bootScreenConnectingPulse() {
  if (!s_connecting_text_drawn) {
    drawConnectingText();
  }
  if (!s_sonar_guides_drawn) {
    drawSonarGuides();
  }
  eraseSonarPing();
  advanceSonarPing();
  drawSonarPing();
}

void bootScreenShowPortalHint() {
  snprintf(s_portal_ip_alt, sizeof(s_portal_ip_alt), "or %s", config::kPortalIp);
  const TextLine lines[] = {
      {"Network setup", displayFontTitle()},
      {"1. Join network:", displayFontBody()},
      {config::kPortalApName, displayFontTitle()},
      {"2. Open in browser:", displayFontBody()},
      {config::kPortalHostUrl, displayFontTitle()},
      {s_portal_ip_alt, displayFontBody()},
  };
  drawTextBlock(config::kColorBlack, config::kTextOnBlack, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void bootScreenShowConnectFailed() {
  const TextLine lines[] = {
      {"Could not connect", displayFontTitle()},
      {"Check Wi-Fi password", displayFontBody()},
      {"and signal strength.", displayFontBody()},
      {"Hold knob 5 sec", displayFontBody()},
      {"to reset Wi-Fi", displayFontBody()},
  };
  drawTextBlock(config::kColorBlack, config::kTextOnBlack, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void bootScreenShowWifiCleared() {
  const TextLine lines[] = {
      {"Resetting Wi-Fi", displayFontTitle()},
  };
  drawTextBlock(config::kColorBlack, config::kTextOnBlack, lines,
                sizeof(lines) / sizeof(lines[0]));
}

namespace {

constexpr int kWifiResetCx = config::kDisplayWidth / 2;
constexpr int kWifiResetCy = config::kDisplayHeight / 2;
constexpr int kWifiResetRingRadiusPx = kWifiResetCx - 2;
constexpr float kWifiResetRingHalfWidth = 2.0f;
constexpr int kWifiResetRingDegStep = 2;
constexpr float kWifiResetDegToRad = 0.01745329252f;

bool s_wifi_reset_ui_active = false;
int s_wifi_reset_last_sec = -1;
int s_wifi_reset_last_deg = -1;
char s_wifi_reset_line[36];

void wifiResetRingPoint(float deg_cw_from_top, int* x, int* y) {
  const float rad = deg_cw_from_top * kWifiResetDegToRad;
  *x = kWifiResetCx +
       static_cast<int>(lroundf(sinf(rad) * static_cast<float>(kWifiResetRingRadiusPx)));
  *y = kWifiResetCy -
       static_cast<int>(lroundf(cosf(rad) * static_cast<float>(kWifiResetRingRadiusPx)));
}

uint16_t wifiResetRingColor() { return tft.color565(26, 156, 60); }

void drawWifiResetRingArc(float start_deg, float end_deg, uint16_t color) {
  if (end_deg <= start_deg + 0.05f) {
    return;
  }
  const float step = static_cast<float>(kWifiResetRingDegStep);
  float a = start_deg;
  int px = 0;
  int py = 0;
  wifiResetRingPoint(a, &px, &py);
  for (float b = start_deg + step; b < end_deg; b += step) {
    int nx = 0;
    int ny = 0;
    wifiResetRingPoint(b, &nx, &ny);
    tft.drawWideLine(static_cast<int16_t>(px), static_cast<int16_t>(py),
                     static_cast<int16_t>(nx), static_cast<int16_t>(ny),
                     kWifiResetRingHalfWidth, color);
    px = nx;
    py = ny;
  }
  int nx = 0;
  int ny = 0;
  wifiResetRingPoint(end_deg, &nx, &ny);
  tft.drawWideLine(static_cast<int16_t>(px), static_cast<int16_t>(py),
                   static_cast<int16_t>(nx), static_cast<int16_t>(ny),
                   kWifiResetRingHalfWidth, color);
}

void paintWifiResetCountdown(int sec, int rem_deg) {
  snprintf(s_wifi_reset_line, sizeof(s_wifi_reset_line), "Resetting Wi-Fi in %d", sec);

  tft.beginOffscreen();
  tft.fillScreen(config::kColorBlack);
  tft.setTextColor(config::kTextOnBlack, config::kColorBlack);
  tft.setTextDatum(TextDatum::MiddleCenter);

  displayFontApply(tft, displayFontBody());
  tft.drawString(s_wifi_reset_line, kWifiResetCx, kWifiResetCy);

  if (rem_deg > 0) {
    drawWifiResetRingArc(0.0f, static_cast<float>(rem_deg), wifiResetRingColor());
  }
  tft.endOffscreen();
}

}  // namespace

void bootScreenWifiResetCountdownTick(unsigned long held_ms, unsigned long total_ms) {
  if (total_ms == 0) {
    return;
  }
  if (held_ms > total_ms) {
    held_ms = total_ms;
  }

  const unsigned long rem_ms = total_ms - held_ms;
  int sec = static_cast<int>((rem_ms + 999UL) / 1000UL);
  if (sec < 1) {
    sec = 1;
  }

  float frac = static_cast<float>(rem_ms) / static_cast<float>(total_ms);
  if (frac < 0.0f) {
    frac = 0.0f;
  }
  if (frac > 1.0f) {
    frac = 1.0f;
  }
  int deg = static_cast<int>(lroundf(frac * 360.0f));
  deg = (deg / kWifiResetRingDegStep) * kWifiResetRingDegStep;

  if (!s_wifi_reset_ui_active || sec != s_wifi_reset_last_sec || deg != s_wifi_reset_last_deg) {
    paintWifiResetCountdown(sec, deg);
    s_wifi_reset_ui_active = true;
    s_wifi_reset_last_sec = sec;
    s_wifi_reset_last_deg = deg;
  }
}

void bootScreenWifiResetCountdownCancel() {
  s_wifi_reset_ui_active = false;
  s_wifi_reset_last_sec = -1;
  s_wifi_reset_last_deg = -1;
}

bool bootScreenWifiResetCountdownActive() { return s_wifi_reset_ui_active; }
