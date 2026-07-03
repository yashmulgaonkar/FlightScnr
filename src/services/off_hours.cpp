#include "services/off_hours.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"

namespace services::offhours {
namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kEnabledKey[] = "night_en";
constexpr char kModeKey[] = "night_mode";
constexpr char kStartKey[] = "night_start";
constexpr char kEndKey[] = "night_end";

bool s_enabled = false;
Mode s_mode = Mode::Dim;
uint16_t s_start_min = config::kOffHoursDefaultStartMin;
uint16_t s_end_min = config::kOffHoursDefaultEndMin;

uint16_t parseTimeStr(const char* str, uint16_t fallback) {
  if (str == nullptr || str[0] == '\0') {
    return fallback;
  }
  const char* colon = strchr(str, ':');
  if (colon == nullptr) {
    return fallback;
  }
  int h = atoi(str);
  int m = atoi(colon + 1);
  if (h < 0 || h > 23 || m < 0 || m > 59) {
    return fallback;
  }
  return static_cast<uint16_t>(h * 60 + m);
}

uint16_t currentMinuteOfDay() {
  struct tm local {};
  const time_t now = time(nullptr);
  if (localtime_r(&now, &local) == nullptr) {
    return UINT16_MAX;
  }
  return static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
}

void persist() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kEnabledKey, s_enabled);
    prefs.putUChar(kModeKey, static_cast<uint8_t>(s_mode));
    prefs.putUShort(kStartKey, s_start_min);
    prefs.putUShort(kEndKey, s_end_min);
    prefs.end();
  }
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  s_enabled = prefs.getBool(kEnabledKey, false);
  const uint8_t mode_raw = prefs.getUChar(kModeKey, 0);
  s_mode = (mode_raw == 1) ? Mode::DisplayOff : Mode::Dim;
  s_start_min = prefs.getUShort(kStartKey, config::kOffHoursDefaultStartMin);
  s_end_min = prefs.getUShort(kEndKey, config::kOffHoursDefaultEndMin);
  prefs.end();
}

bool active() {
  if (!s_enabled) {
    return false;
  }
  const uint16_t now_min = currentMinuteOfDay();
  if (now_min == UINT16_MAX) {
    return false;
  }
  if (s_start_min == s_end_min) {
    return false;
  }
  if (s_start_min < s_end_min) {
    return now_min >= s_start_min && now_min < s_end_min;
  }
  // Overnight wraparound (e.g. 22:00 - 07:00)
  return now_min >= s_start_min || now_min < s_end_min;
}

Mode mode() { return s_mode; }
bool enabled() { return s_enabled; }
uint16_t startMinute() { return s_start_min; }
uint16_t endMinute() { return s_end_min; }

void saveFromForm(const char* enable_checkbox, const char* mode_str,
                  const char* start_str, const char* end_str) {
  s_enabled = (enable_checkbox != nullptr && enable_checkbox[0] == 'T');
  if (mode_str != nullptr) {
    s_mode = (atoi(mode_str) == 1) ? Mode::DisplayOff : Mode::Dim;
  }
  s_start_min = parseTimeStr(start_str, s_start_min);
  s_end_min = parseTimeStr(end_str, s_end_min);
  persist();
  Serial.printf("[offhours] saved en=%d mode=%d start=%02u:%02u end=%02u:%02u\n",
                s_enabled ? 1 : 0, static_cast<int>(s_mode),
                s_start_min / 60, s_start_min % 60,
                s_end_min / 60, s_end_min % 60);
}

}  // namespace services::offhours
