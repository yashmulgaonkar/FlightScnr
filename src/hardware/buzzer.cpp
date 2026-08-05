#include "hardware/buzzer.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

#include <cctype>
#include <cstdlib>

#include "hardware/pin_config.h"

#if FLIGHTSCNR_HAS_HAPTIC
#include "SensorDRV2605.hpp"
#endif

namespace hardware {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kEnabledKey[] = "beep_en";
constexpr char kToneKey[] = "beep_vol";

constexpr int kLedcChannel = 2;
constexpr int kLedcResolution = 8;
constexpr uint16_t kToneHzDefault = 2000;

constexpr char kToneLetters[] = {'A', 'B', 'C', 'D', 'E'};
constexpr size_t kToneLevelCount = sizeof(kToneLetters) / sizeof(kToneLetters[0]);
constexpr uint8_t kDefaultToneIndex = 0;  // A

// LilyGO T-Encoder-Pro factory (Lvgl_CIT): ledcAttach @ 2000 Hz, duty 127 (~50%),
// ledcChangeFrequency per step (1500/2000/2500 Hz). Never use duty 255 (DC, no tone).
struct BeepProfile {
  uint16_t freq_hz;
  uint8_t duty;
  uint8_t duration_ms;
};

constexpr BeepProfile kBeepProfiles[] = {
    {1500, 10, 5},    // A
    {1750, 28, 12},   // B
    {2000, 64, 22},   // C
    {2250, 96, 28},   // D
    {2500, 127, 32},  // E
};
static_assert(sizeof(kBeepProfiles) / sizeof(kBeepProfiles[0]) == kToneLevelCount,
              "beep profiles must match tone levels");

#if FLIGHTSCNR_HAS_HAPTIC
// Waveshare demo library 1 effect IDs (soft → strong). Skip Soft Bump — easy to miss.
constexpr uint8_t kHapticClickEffects[] = {3, 2, 1, 14, 15};
static_assert(sizeof(kHapticClickEffects) / sizeof(kHapticClickEffects[0]) ==
                  kToneLevelCount,
              "haptic click effects must match tone levels");
constexpr uint8_t kHapticAlertEffect = 14;  // Strong Buzz 100%
constexpr uint8_t kHapticBootEffect = 1;    // Strong Click — hardware self-test
#endif

constexpr uint8_t kLegacyTonePercents[] = {20, 40, 60, 80, 100};

bool s_enabled = true;
uint8_t s_tone_index = kDefaultToneIndex;
bool s_playing = false;
unsigned long s_stop_at_ms = 0;

constexpr uint16_t kAlertFreqHz = 2500;
constexpr uint8_t kAlertDuty = 127;
constexpr uint8_t kAlertBeepMs = 60;
constexpr uint8_t kAlertIntervalMs = 280;  // buzz needs more gap than a click
constexpr uint8_t kAlertBeepCount = 3;

uint8_t s_alert_beeps_remaining = 0;
unsigned long s_alert_next_ms = 0;

#if FLIGHTSCNR_HAS_HAPTIC
SensorDRV2605 s_drv;
bool s_haptic_ready = false;

void hapticPlayEffect(uint8_t effect, bool require_ui_beep) {
  if (!s_haptic_ready || effect == 0) {
    return;
  }
  if (require_ui_beep && !s_enabled) {
    return;
  }
  // Same sequence as Waveshare 03_DRV2605_Test.
  s_drv.stop();
  s_drv.setMode(DRV2605_MODE_INTTRIG);
  s_drv.setWaveform(0, effect);
  s_drv.setWaveform(1, 0);
  s_drv.run();
}

bool hapticInit() {
  // Match Waveshare demo: SensorDRV2605 init (ERM open-loop) + library 1 + INTTRIG.
  if (!s_drv.init(Wire, IIC_SDA, IIC_SCL, DRV2605_SLAVE_ADDRESS)) {
    Serial.println("Haptic: DRV2605 not found");
    return false;
  }
  s_drv.selectLibrary(1);
  s_drv.setMode(DRV2605_MODE_INTTRIG);
  Serial.println("Haptic: DRV2605 ready (lib 1)");
  return true;
}
#endif

size_t toneIndexFromLegacyPercent(uint8_t pct) {
  for (size_t i = 0; i < kToneLevelCount; ++i) {
    if (kLegacyTonePercents[i] == pct) {
      return i;
    }
  }
  size_t best = 0;
  uint8_t best_delta = 255;
  for (size_t i = 0; i < kToneLevelCount; ++i) {
    const uint8_t level = kLegacyTonePercents[i];
    const uint8_t delta =
        (pct > level) ? static_cast<uint8_t>(pct - level) : static_cast<uint8_t>(level - pct);
    if (delta < best_delta) {
      best_delta = delta;
      best = i;
    }
  }
  return best;
}

size_t toneIndexFromStored(uint8_t stored) {
  if (stored < kToneLevelCount) {
    return stored;
  }
  return toneIndexFromLegacyPercent(stored);
}

size_t toneIndexFromForm(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return kDefaultToneIndex;
  }

  if (value[1] == '\0') {
    char letter = static_cast<char>(toupper(static_cast<unsigned char>(value[0])));
    if (letter >= 'A' && letter <= 'E') {
      return static_cast<size_t>(letter - 'A');
    }
  }

  const int pct = static_cast<int>(lroundf(strtof(value, nullptr)));
  if (pct <= 0) {
    return 0;
  }
  if (pct >= static_cast<int>(kLegacyTonePercents[kToneLevelCount - 1])) {
    return kToneLevelCount - 1;
  }
  return toneIndexFromLegacyPercent(static_cast<uint8_t>(pct));
}

void persistSettings() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kEnabledKey, s_enabled);
    prefs.putUChar(kToneKey, s_tone_index);
    prefs.end();
  }
}

const BeepProfile& beepProfileForIndex(size_t index) {
  return kBeepProfiles[index];
}

void startTone() {
  if (!s_enabled) {
    return;
  }

#if FLIGHTSCNR_HAS_HAPTIC
  if (s_haptic_ready) {
    const uint8_t effect = kHapticClickEffects[s_tone_index];
    hapticPlayEffect(effect, /*require_ui_beep=*/true);
    return;
  }
#endif

#if FLIGHTSCNR_HAS_BUZZER
  const BeepProfile& profile = beepProfileForIndex(s_tone_index);
  ledcChangeFrequency(kLedcChannel, profile.freq_hz, kLedcResolution);
  ledcWrite(kLedcChannel, profile.duty);
  s_playing = true;
  s_stop_at_ms = millis() + profile.duration_ms;
#else
  (void)beepProfileForIndex;
#endif
}

}  // namespace

void buzzerInit() {
#if FLIGHTSCNR_HAS_BUZZER
  ledcSetup(kLedcChannel, kToneHzDefault, kLedcResolution);
  ledcAttachPin(BUZZER_DATA, kLedcChannel);
  ledcWrite(kLedcChannel, 0);
#endif
#if FLIGHTSCNR_HAS_HAPTIC
  s_haptic_ready = hapticInit();
#endif
}

void buzzerBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  s_enabled = prefs.getBool(kEnabledKey, true);
  const uint8_t stored = prefs.getUChar(kToneKey, kDefaultToneIndex);
  prefs.end();
  s_tone_index = static_cast<uint8_t>(toneIndexFromStored(stored));
#if FLIGHTSCNR_HAS_HAPTIC
  Serial.printf("Haptic: UI vibe %s intensity %s\n", s_enabled ? "on" : "off",
                buzzerIntensityLabel());
  // Always pulse once at boot so a muted UI vibe can't hide a dead motor.
  if (s_haptic_ready) {
    hapticPlayEffect(kHapticBootEffect, /*require_ui_beep=*/false);
  }
#endif
}

bool buzzerEnabled() { return s_enabled; }

char buzzerToneLetter() { return kToneLetters[s_tone_index]; }

uint8_t buzzerIntensityLevel() {
  return static_cast<uint8_t>(s_tone_index + 1);
}

const char* buzzerIntensityLabel() {
  static constexpr const char* kLabels[] = {"20%", "40%", "60%", "80%", "100%"};
  return kLabels[s_tone_index < kToneLevelCount ? s_tone_index : 0];
}

void buzzerSetEnabled(bool enabled) {
  if (s_enabled == enabled) {
    return;
  }
  s_enabled = enabled;
  persistSettings();
#if FLIGHTSCNR_HAS_HAPTIC
  Serial.printf("Vibration: %s\n", s_enabled ? "on" : "off");
#else
  Serial.printf("UI beep: %s\n", s_enabled ? "on" : "off");
#endif
}

void buzzerToneStep(int8_t delta) {
  if (delta == 0) {
    return;
  }

  size_t idx = s_tone_index;
  if (delta > 0) {
    idx = (idx + 1) % kToneLevelCount;
  } else {
    idx = (idx == 0) ? kToneLevelCount - 1 : idx - 1;
  }
  s_tone_index = static_cast<uint8_t>(idx);
  persistSettings();
#if FLIGHTSCNR_HAS_HAPTIC
  Serial.printf("Vibration intensity: %s\n", buzzerIntensityLabel());
#else
  Serial.printf("Beep tone: %c\n", buzzerToneLetter());
#endif
  buzzerClick();
}

void buzzerClick() {
  startTone();
}

void buzzerPoll() {
#if FLIGHTSCNR_HAS_BUZZER
  if (s_playing && millis() >= s_stop_at_ms) {
    ledcWrite(kLedcChannel, 0);
    s_playing = false;
  }
#endif

  if (s_alert_beeps_remaining == 0) {
    return;
  }
  if (millis() < s_alert_next_ms) {
    return;
  }

#if FLIGHTSCNR_HAS_HAPTIC
  if (s_haptic_ready) {
    // Alerts always vibrate — UI Beep only gates clicks, not watch/mil/emrg.
    hapticPlayEffect(kHapticAlertEffect, /*require_ui_beep=*/false);
    --s_alert_beeps_remaining;
    s_alert_next_ms = millis() + kAlertIntervalMs;
    return;
  }
#endif

#if FLIGHTSCNR_HAS_BUZZER
  ledcChangeFrequency(kLedcChannel, kAlertFreqHz, kLedcResolution);
  ledcWrite(kLedcChannel, kAlertDuty);
  s_playing = true;
  s_stop_at_ms = millis() + kAlertBeepMs;
  --s_alert_beeps_remaining;
  s_alert_next_ms = millis() + kAlertIntervalMs;
#else
  s_alert_beeps_remaining = 0;
#endif
}

void buzzerAlert() {
#if FLIGHTSCNR_HAS_HAPTIC
  if (s_haptic_ready) {
    s_alert_beeps_remaining = kAlertBeepCount;
    s_alert_next_ms = 0;
    return;
  }
#endif
  if (!s_enabled) {
    return;
  }
  s_alert_beeps_remaining = kAlertBeepCount;
  s_alert_next_ms = 0;
}

void saveBeepEnabledFromForm(const char* checkbox_value) {
  const bool enabled = checkbox_value != nullptr && checkbox_value[0] == 'T';
  s_enabled = enabled;
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kEnabledKey, s_enabled);
    prefs.end();
  }
}

void saveBeepToneFromForm(const char* value) {
  s_tone_index = static_cast<uint8_t>(toneIndexFromForm(value));
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kToneKey, s_tone_index);
    prefs.end();
  }
}

}  // namespace hardware
