#include "hardware/buzzer.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstdlib>

#include "hardware/pin_config.h"

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

constexpr uint8_t kLegacyTonePercents[] = {20, 40, 60, 80, 100};

bool s_enabled = true;
uint8_t s_tone_index = kDefaultToneIndex;
bool s_playing = false;
unsigned long s_stop_at_ms = 0;

constexpr uint16_t kAlertFreqHz = 2500;
constexpr uint8_t kAlertDuty = 127;
constexpr uint8_t kAlertBeepMs = 60;
constexpr uint8_t kAlertIntervalMs = 140;  // beep + gap
constexpr uint8_t kAlertBeepCount = 3;

uint8_t s_alert_beeps_remaining = 0;
unsigned long s_alert_next_ms = 0;

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

  const BeepProfile& profile = beepProfileForIndex(s_tone_index);
  ledcChangeFrequency(kLedcChannel, profile.freq_hz, kLedcResolution);
  ledcWrite(kLedcChannel, profile.duty);
  s_playing = true;
  s_stop_at_ms = millis() + profile.duration_ms;
}

}  // namespace

void buzzerInit() {
  ledcSetup(kLedcChannel, kToneHzDefault, kLedcResolution);
  ledcAttachPin(BUZZER_DATA, kLedcChannel);
  ledcWrite(kLedcChannel, 0);
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
}

bool buzzerEnabled() { return s_enabled; }

char buzzerToneLetter() { return kToneLetters[s_tone_index]; }

void buzzerSetEnabled(bool enabled) {
  if (s_enabled == enabled) {
    return;
  }
  s_enabled = enabled;
  persistSettings();
  Serial.printf("UI beep: %s\n", s_enabled ? "on" : "off");
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
  Serial.printf("Beep tone: %c\n", buzzerToneLetter());
}

void buzzerClick() {
  startTone();
}

void buzzerPoll() {
  if (s_playing && millis() >= s_stop_at_ms) {
    ledcWrite(kLedcChannel, 0);
    s_playing = false;
  }

  if (s_alert_beeps_remaining == 0) {
    return;
  }
  if (millis() < s_alert_next_ms) {
    return;
  }
  ledcChangeFrequency(kLedcChannel, kAlertFreqHz, kLedcResolution);
  ledcWrite(kLedcChannel, kAlertDuty);
  s_playing = true;
  s_stop_at_ms = millis() + kAlertBeepMs;
  --s_alert_beeps_remaining;
  s_alert_next_ms = millis() + kAlertIntervalMs;
}

void buzzerAlert() {
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
