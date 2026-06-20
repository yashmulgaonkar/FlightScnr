#include "ui/display_prefs.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>

namespace ui {
namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kDetailTimeoutKey[] = "detail_to";
constexpr char kSweepLineKey[] = "sweep_en";

constexpr uint8_t kDefaultDetailTimeoutSec = 10;

/** 0 = manual; otherwise 10, 20, or 30. */
uint8_t s_flight_detail_timeout_sec = kDefaultDetailTimeoutSec;
bool s_sweep_line_enabled = true;

constexpr uint8_t kTimeoutOptions[] = {0, 10, 20, 30};
constexpr size_t kTimeoutOptionCount = sizeof(kTimeoutOptions) / sizeof(kTimeoutOptions[0]);

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

void persistDetailTimeout() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kDetailTimeoutKey, s_flight_detail_timeout_sec);
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

int timeoutOptionIndex(uint8_t sec) {
  for (size_t i = 0; i < kTimeoutOptionCount; ++i) {
    if (kTimeoutOptions[i] == sec) {
      return static_cast<int>(i);
    }
  }
  return 1;
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
  s_sweep_line_enabled = prefs.getBool(kSweepLineKey, true);
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

}  // namespace ui
