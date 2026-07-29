#include "ui/display_prefs.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>

#include "ui/radar_display.h"

namespace ui {
namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kDetailTimeoutKey[] = "detail_to";
constexpr char kClockTimeoutKey[] = "clock_to";
constexpr char kSweepLineKey[] = "sweep_en";
constexpr char kHideBlipDetailsKey[] = "hide_btag";
constexpr char kIdleClockKey[] = "idle_clk";

constexpr uint8_t kDefaultDetailTimeoutSec = 10;
constexpr uint8_t kDefaultClockTimeoutSec = 10;

/** 0 = manual; otherwise 10, 20, or 30. */
uint8_t s_flight_detail_timeout_sec = kDefaultDetailTimeoutSec;
/** 0 = manual; otherwise 5, 10, or 15. */
uint8_t s_clock_weather_timeout_sec = kDefaultClockTimeoutSec;
bool s_sweep_line_enabled = true;
bool s_hide_blip_details = false;
bool s_auto_idle_clock_enabled = true;

constexpr uint8_t kTimeoutOptions[] = {0, 10, 20, 30};
constexpr size_t kTimeoutOptionCount = sizeof(kTimeoutOptions) / sizeof(kTimeoutOptions[0]);
constexpr uint8_t kClockTimeoutOptions[] = {0, 5, 10, 15};
constexpr size_t kClockTimeoutOptionCount =
    sizeof(kClockTimeoutOptions) / sizeof(kClockTimeoutOptions[0]);

bool formCheckboxOn(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  if ((value[0] == 'F' || value[0] == 'f') && value[1] == '\0') {
    return false;
  }
  if ((value[0] == 'T' || value[0] == 't') && value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

bool isValidTimeoutSec(uint8_t sec) {
  for (size_t i = 0; i < kTimeoutOptionCount; ++i) {
    if (kTimeoutOptions[i] == sec) {
      return true;
    }
  }
  return false;
}

bool isValidClockTimeoutSec(uint8_t sec) {
  for (size_t i = 0; i < kClockTimeoutOptionCount; ++i) {
    if (kClockTimeoutOptions[i] == sec) {
      return true;
    }
  }
  return false;
}

void persistDetailTimeout() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kDetailTimeoutKey, s_flight_detail_timeout_sec);
    prefs.end();
  }
}

void persistClockTimeout() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kClockTimeoutKey, s_clock_weather_timeout_sec);
    prefs.end();
  }
}

void persistSweepLine() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kSweepLineKey, s_sweep_line_enabled);
    prefs.end();
  }
}

void persistHideBlipDetails() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kHideBlipDetailsKey, s_hide_blip_details);
    prefs.end();
  }
}

void persistAutoIdleClock() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kIdleClockKey, s_auto_idle_clock_enabled);
    prefs.end();
  }
}

int timeoutOptionIndex(uint8_t sec) {
  for (size_t i = 0; i < kTimeoutOptionCount; ++i) {
    if (kTimeoutOptions[i] == sec) {
      return static_cast<int>(i);
    }
  }
  return 1;
}

int clockTimeoutOptionIndex(uint8_t sec) {
  for (size_t i = 0; i < kClockTimeoutOptionCount; ++i) {
    if (kClockTimeoutOptions[i] == sec) {
      return static_cast<int>(i);
    }
  }
  return 2;
}

}  // namespace

void displayPrefsBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  const uint8_t stored = prefs.getUChar(kDetailTimeoutKey, kDefaultDetailTimeoutSec);
  s_flight_detail_timeout_sec =
      isValidTimeoutSec(stored) ? stored : kDefaultDetailTimeoutSec;
  const uint8_t clock_stored = prefs.getUChar(kClockTimeoutKey, kDefaultClockTimeoutSec);
  s_clock_weather_timeout_sec =
      isValidClockTimeoutSec(clock_stored) ? clock_stored : kDefaultClockTimeoutSec;
  s_sweep_line_enabled = prefs.getBool(kSweepLineKey, true);
  s_hide_blip_details = prefs.getBool(kHideBlipDetailsKey, false);
  s_auto_idle_clock_enabled = prefs.getBool(kIdleClockKey, true);
  prefs.end();
}

unsigned long displayPrefsFlightDetailTimeoutMs() {
  if (s_flight_detail_timeout_sec == 0) {
    return 0;
  }
  return static_cast<unsigned long>(s_flight_detail_timeout_sec) * 1000UL;
}

const char* displayPrefsFlightDetailTimeoutLabel() {
  switch (s_flight_detail_timeout_sec) {
    case 0:
      return "Manual";
    case 10:
      return "10 sec";
    case 20:
      return "20 sec";
    case 30:
      return "30 sec";
    default:
      return "10 sec";
  }
}

void displayPrefsFlightDetailTimeoutStep(int8_t delta) {
  if (delta == 0) {
    return;
  }
  int idx = timeoutOptionIndex(s_flight_detail_timeout_sec);
  idx = (idx + delta) % static_cast<int>(kTimeoutOptionCount);
  if (idx < 0) {
    idx += static_cast<int>(kTimeoutOptionCount);
  }
  s_flight_detail_timeout_sec = kTimeoutOptions[static_cast<size_t>(idx)];
  persistDetailTimeout();
  Serial.printf("Flight detail timeout: %s\n", displayPrefsFlightDetailTimeoutLabel());
}

void displayPrefsSaveFlightDetailTimeoutFromForm(const char* seconds_str) {
  if (seconds_str == nullptr || seconds_str[0] == '\0') {
    return;
  }
  char* end = nullptr;
  const long v = strtol(seconds_str, &end, 10);
  if (end == seconds_str || (end != nullptr && *end != '\0')) {
    return;
  }
  if (v < 0 || v > 255) {
    return;
  }
  const uint8_t sec = static_cast<uint8_t>(v);
  if (!isValidTimeoutSec(sec)) {
    return;
  }
  s_flight_detail_timeout_sec = sec;
  persistDetailTimeout();
  Serial.printf("Flight detail timeout: %s\n", displayPrefsFlightDetailTimeoutLabel());
}

unsigned long displayPrefsClockWeatherTimeoutMs() {
  if (s_clock_weather_timeout_sec == 0) {
    return 0;
  }
  return static_cast<unsigned long>(s_clock_weather_timeout_sec) * 1000UL;
}

const char* displayPrefsClockWeatherTimeoutLabel() {
  switch (s_clock_weather_timeout_sec) {
    case 0:
      return "Manual";
    case 5:
      return "5 sec";
    case 10:
      return "10 sec";
    case 15:
      return "15 sec";
    default:
      return "10 sec";
  }
}

void displayPrefsClockWeatherTimeoutStep(int8_t delta) {
  if (delta == 0) {
    return;
  }
  int idx = clockTimeoutOptionIndex(s_clock_weather_timeout_sec);
  idx = (idx + delta) % static_cast<int>(kClockTimeoutOptionCount);
  if (idx < 0) {
    idx += static_cast<int>(kClockTimeoutOptionCount);
  }
  s_clock_weather_timeout_sec = kClockTimeoutOptions[static_cast<size_t>(idx)];
  persistClockTimeout();
  Serial.printf("Clock/forecast timeout: %s\n", displayPrefsClockWeatherTimeoutLabel());
}

void displayPrefsSaveClockWeatherTimeoutFromForm(const char* seconds_str) {
  if (seconds_str == nullptr || seconds_str[0] == '\0') {
    return;
  }
  char* end = nullptr;
  const long v = strtol(seconds_str, &end, 10);
  if (end == seconds_str || (end != nullptr && *end != '\0')) {
    return;
  }
  if (v < 0 || v > 255) {
    return;
  }
  const uint8_t sec = static_cast<uint8_t>(v);
  if (!isValidClockTimeoutSec(sec)) {
    return;
  }
  s_clock_weather_timeout_sec = sec;
  persistClockTimeout();
  Serial.printf("Clock/forecast timeout: %s\n", displayPrefsClockWeatherTimeoutLabel());
}

bool displayPrefsSweepLineEnabled() { return s_sweep_line_enabled; }

void displayPrefsToggleSweepLine() {
  s_sweep_line_enabled = !s_sweep_line_enabled;
  persistSweepLine();
  Serial.printf("Radar sweep: %s\n", s_sweep_line_enabled ? "on" : "off");
}

void displayPrefsSaveSweepLineFromForm(const char* checkbox_value) {
  s_sweep_line_enabled = formCheckboxOn(checkbox_value);
  persistSweepLine();
  Serial.printf("Radar sweep: %s\n", s_sweep_line_enabled ? "on" : "off");
}

bool displayPrefsHideBlipDetails() { return s_hide_blip_details; }

void displayPrefsToggleHideBlipDetails() {
  s_hide_blip_details = !s_hide_blip_details;
  persistHideBlipDetails();
  radarDisplayInvalidateAircraft();
  Serial.printf("Hide blip details: %s\n", s_hide_blip_details ? "on" : "off");
}

void displayPrefsSaveHideBlipDetailsFromForm(const char* checkbox_value) {
  const bool next = formCheckboxOn(checkbox_value);
  if (next == s_hide_blip_details) {
    return;
  }
  s_hide_blip_details = next;
  persistHideBlipDetails();
  radarDisplayInvalidateAircraft();
  Serial.printf("Hide blip details: %s\n", s_hide_blip_details ? "on" : "off");
}

bool displayPrefsAutoIdleClockEnabled() { return s_auto_idle_clock_enabled; }

void displayPrefsToggleAutoIdleClock() {
  s_auto_idle_clock_enabled = !s_auto_idle_clock_enabled;
  persistAutoIdleClock();
  Serial.printf("Idle clock: %s\n", s_auto_idle_clock_enabled ? "on" : "off");
}

void displayPrefsSaveAutoIdleClockFromForm(const char* checkbox_value) {
  s_auto_idle_clock_enabled = formCheckboxOn(checkbox_value);
  persistAutoIdleClock();
  Serial.printf("Idle clock: %s\n", s_auto_idle_clock_enabled ? "on" : "off");
}

}  // namespace ui
