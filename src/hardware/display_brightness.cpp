#include "hardware/display_brightness.h"

#include <Preferences.h>
#include <cstdlib>

#include "hardware/display.h"
#include "hardware/pin_config.h"
#include "services/settings_state.h"

namespace hardware {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kBrightPctKey[] = "bright_pct";

constexpr uint8_t kLevels[] = {20, 40, 60, 80, 100};
constexpr size_t kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);

#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
constexpr int kBlLedcChannel = 3;
constexpr int kBlLedcFreqHz = 5000;
constexpr int kBlLedcResolution = 8;
bool s_bl_pwm_ready = false;
#endif

uint8_t s_percent = 100;

size_t levelIndexFor(uint8_t pct) {
  for (size_t i = 0; i < kLevelCount; ++i) {
    if (kLevels[i] == pct) {
      return i;
    }
  }
  return kLevelCount - 1;
}

uint8_t panelLevelForPercent(uint8_t pct) {
  return static_cast<uint8_t>((static_cast<uint16_t>(pct) * 255u + 50u) / 100u);
}

#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
void ensureBacklightPwm() {
  if (s_bl_pwm_ready) {
    return;
  }
  ledcSetup(kBlLedcChannel, kBlLedcFreqHz, kBlLedcResolution);
  ledcAttachPin(LCD_BL, kBlLedcChannel);
  s_bl_pwm_ready = true;
}

void applyBacklightPwm(uint8_t pct) {
  ensureBacklightPwm();
  ledcWrite(kBlLedcChannel, panelLevelForPercent(pct));
}
#endif

}  // namespace

void displayBrightnessBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  const uint8_t stored = prefs.getUChar(kBrightPctKey, 100);
  prefs.end();
  s_percent = kLevels[levelIndexFor(stored)];
}

uint8_t displayBrightnessPercent() { return s_percent; }

void displayApplyBrightness() {
#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  applyBacklightPwm(s_percent);
#else
  Arduino_GFX* const panel = tft.raw();
  if (panel == nullptr) {
    return;
  }
  panel->Display_Brightness(panelLevelForPercent(s_percent));
#endif
}

void displayBrightnessStep(int8_t delta) {
  if (delta == 0) {
    return;
  }

  size_t idx = levelIndexFor(s_percent);
  if (delta > 0) {
    idx = (idx + 1) % kLevelCount;
  } else {
    idx = (idx == 0) ? kLevelCount - 1 : idx - 1;
  }
  s_percent = kLevels[idx];

  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kBrightPctKey, s_percent);
    prefs.end();
  }

  displayApplyBrightness();
  settingsStateBump();
  Serial.printf("Brightness: %u%%\n", static_cast<unsigned>(s_percent));
}

void displayBrightnessSaveFromForm(const char* percent_str) {
  if (percent_str == nullptr || percent_str[0] == '\0') {
    return;
  }
  char* end = nullptr;
  const long v = strtol(percent_str, &end, 10);
  if (end == percent_str || (end != nullptr && *end != '\0')) {
    return;
  }
  if (v < 0 || v > 100) {
    return;
  }
  s_percent = kLevels[levelIndexFor(static_cast<uint8_t>(v))];

  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kBrightPctKey, s_percent);
    prefs.end();
  }
  settingsStateBump();
  Serial.printf("Brightness: %u%%\n", static_cast<unsigned>(s_percent));
}

void displayBrightnessOverride(uint8_t percent) {
#if FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM
  applyBacklightPwm(percent);
#else
  Arduino_GFX* const panel = tft.raw();
  if (panel == nullptr) {
    return;
  }
  panel->Display_Brightness(panelLevelForPercent(percent));
#endif
}

void displayBrightnessRestore() { displayApplyBrightness(); }

}  // namespace hardware
