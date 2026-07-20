#include "services/clock_time.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "services/clock_format.h"

namespace services::clock {

namespace {

constexpr char kNs[] = "flightscnr";
constexpr char kTzOffsetKey[] = "clk_tz_sec";
constexpr char kUse24hKey[] = "clk_24h";
constexpr char kDateFmtKey[] = "clk_datefmt";
constexpr char kAutoTzKey[] = "clk_auto_tz";
constexpr char kPosixTzKey[] = "clk_posix";
constexpr char kIanaTzKey[] = "clk_iana";
constexpr char kResolvedLatKey[] = "clk_tz_lat";
constexpr char kResolvedLonKey[] = "clk_tz_lon";

constexpr int32_t kMinOffsetSec = -12 * 3600;
constexpr int32_t kMaxOffsetSec = 14 * 3600;
constexpr time_t kMinValidEpoch = 1600000000;
constexpr size_t kMaxPosixLen = 48;
constexpr size_t kMaxIanaLen = 40;

int32_t s_tz_offset_sec = 0;
bool s_use_24h = false;
bool s_use_numeric_date = false;
bool s_auto_timezone = true;
char s_posix_tz[kMaxPosixLen + 1] = {0};
char s_iana_name[kMaxIanaLen + 1] = {0};
double s_resolved_lat = 0.0;
double s_resolved_lon = 0.0;

bool portalCheckboxChecked(const char* value) {
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

void copyBlob(char* dest, const char* src, size_t cap) {
  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, cap - 1);
  dest[cap - 1] = '\0';
}

bool coordsMatch(double a_lat, double a_lon, double b_lat, double b_lon) {
  constexpr double kEps = 1e-5;
  return std::fabs(a_lat - b_lat) < kEps && std::fabs(a_lon - b_lon) < kEps;
}

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void stripLeadingZeroHour(char* out) {
  if (out != nullptr && out[0] == '0' && out[1] != '\0') {
    memmove(out, out + 1, strlen(out));
  }
}

void persistOffset() {
  Preferences prefs;
  if (prefs.begin(kNs, false)) {
    prefs.putInt(kTzOffsetKey, s_tz_offset_sec);
    prefs.end();
  }
}

void persistFormat() {
  Preferences prefs;
  if (prefs.begin(kNs, false)) {
    prefs.putBool(kUse24hKey, s_use_24h);
    prefs.end();
  }
}

void persistDateFormat() {
  Preferences prefs;
  if (prefs.begin(kNs, false)) {
    prefs.putBool(kDateFmtKey, s_use_numeric_date);
    prefs.end();
  }
}

void persistAutoState() {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  prefs.putBool(kAutoTzKey, s_auto_timezone);
  prefs.putString(kPosixTzKey, s_posix_tz);
  prefs.putString(kIanaTzKey, s_iana_name);
  prefs.putDouble(kResolvedLatKey, s_resolved_lat);
  prefs.putDouble(kResolvedLonKey, s_resolved_lon);
  prefs.end();
}

void applyManualNtpConfig() {
  configTime(s_tz_offset_sec, 0, config::kNtpServer1, config::kNtpServer2);
}

void applyAutoNtpConfig() {
  if (s_posix_tz[0] == '\0') {
    applyManualNtpConfig();
    return;
  }
  configTzTime(s_posix_tz, config::kNtpServer1, config::kNtpServer2);
}

void applyNtpConfig() {
  if (s_auto_timezone && s_posix_tz[0] != '\0') {
    applyAutoNtpConfig();
  } else {
    applyManualNtpConfig();
  }
}

bool timeValid() {
  const time_t now = time(nullptr);
  return now >= kMinValidEpoch;
}

int32_t utcOffsetFromEpoch(time_t utc) {
  struct tm loc {};
  struct tm gmt {};
  if (localtime_r(&utc, &loc) == nullptr || gmtime_r(&utc, &gmt) == nullptr) {
    return s_tz_offset_sec;
  }
  int32_t offset = (loc.tm_hour - gmt.tm_hour) * 3600 + (loc.tm_min - gmt.tm_min) * 60 +
                   (loc.tm_sec - gmt.tm_sec);
  offset += (loc.tm_yday - gmt.tm_yday) * 86400;
  offset += (loc.tm_year - gmt.tm_year) * 31557600;
  return offset;
}

void setTimezoneOffsetSec(int32_t offset_sec) {
  if (offset_sec < kMinOffsetSec) {
    offset_sec = kMinOffsetSec;
  } else if (offset_sec > kMaxOffsetSec) {
    offset_sec = kMaxOffsetSec;
  }
  s_tz_offset_sec = offset_sec;
  persistOffset();
  applyNtpConfig();
}

void setUse24Hour(bool use_24h) {
  s_use_24h = use_24h;
  persistFormat();
}

void formatManualTimezoneLabel(char* out, size_t len) {
  const int32_t off = s_tz_offset_sec;
  const int sign = off >= 0 ? 1 : -1;
  const int32_t abs_sec = off >= 0 ? off : -off;
  const int h = abs_sec / 3600;
  const int m = (abs_sec % 3600) / 60;
  if (m == 0) {
    snprintf(out, len, "UTC%+d", sign * h);
  } else {
    snprintf(out, len, "UTC%+d:%02d", sign * h, m);
  }
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (!prefs.begin(kNs, true)) {
    s_tz_offset_sec = 0;
    s_use_24h = false;
    s_use_numeric_date = false;
    s_auto_timezone = true;
    return;
  }
  s_tz_offset_sec = prefs.getInt(kTzOffsetKey, 0);
  s_use_24h = prefs.getBool(kUse24hKey, false);
  s_use_numeric_date = prefs.getBool(kDateFmtKey, false);
  s_auto_timezone = prefs.getBool(kAutoTzKey, true);
  copyBlob(s_posix_tz, prefs.getString(kPosixTzKey).c_str(), sizeof(s_posix_tz));
  copyBlob(s_iana_name, prefs.getString(kIanaTzKey).c_str(), sizeof(s_iana_name));
  s_resolved_lat = prefs.getDouble(kResolvedLatKey, 0.0);
  s_resolved_lon = prefs.getDouble(kResolvedLonKey, 0.0);
  prefs.end();

  if (s_tz_offset_sec < kMinOffsetSec) {
    s_tz_offset_sec = kMinOffsetSec;
  } else if (s_tz_offset_sec > kMaxOffsetSec) {
    s_tz_offset_sec = kMaxOffsetSec;
  }
}

void startNtp() {
  applyNtpConfig();
  if (s_auto_timezone && s_posix_tz[0] != '\0') {
    Serial.printf("Clock: NTP %s / %s (auto %s)\n", config::kNtpServer1, config::kNtpServer2,
                  s_iana_name[0] != '\0' ? s_iana_name : s_posix_tz);
  } else {
    Serial.printf("Clock: NTP %s / %s (UTC%+d manual)\n", config::kNtpServer1,
                  config::kNtpServer2, static_cast<int>(s_tz_offset_sec / 3600));
  }
}

uint32_t localMinuteStamp() {
  if (!timeValid()) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(time(nullptr) / 60);
}

int32_t timezoneOffsetSec() {
  if (timeValid()) {
    return utcOffsetFromEpoch(time(nullptr));
  }
  return s_tz_offset_sec;
}

int32_t localDayIndex() {
  if (!timeValid()) {
    return -1;
  }
  struct tm local {};
  const time_t now = time(nullptr);
  if (localtime_r(&now, &local) == nullptr) {
    return -1;
  }
  return static_cast<int32_t>(
      daysFromCivil(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                    static_cast<unsigned>(local.tm_mday)));
}

bool useAutoTimezone() { return s_auto_timezone; }

void setAutoTimezone(bool enabled) {
  if (s_auto_timezone == enabled) {
    return;
  }
  s_auto_timezone = enabled;
  persistAutoState();
  applyNtpConfig();
}

void saveAutoTimezoneFromForm(const char* auto_timezone) {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  s_auto_timezone = portalCheckboxChecked(auto_timezone);
  prefs.putBool(kAutoTzKey, s_auto_timezone);
  prefs.end();
  applyNtpConfig();
}

void stepTimezoneHours(int8_t delta) {
  if (delta == 0) {
    return;
  }
  if (s_auto_timezone) {
    s_auto_timezone = false;
    s_tz_offset_sec = timezoneOffsetSec();
    persistAutoState();
  }
  setTimezoneOffsetSec(s_tz_offset_sec + static_cast<int32_t>(delta) * 3600);
}

bool use24Hour() { return s_use_24h; }

void toggleHourFormat() {
  setUse24Hour(!s_use_24h);
}

void saveHourFormatFromForm(const char* h24_checkbox) {
  setUse24Hour(portalCheckboxChecked(h24_checkbox));
}

bool useNumericDate() { return s_use_numeric_date; }

void saveDateFormatFromForm(const char* numeric_checkbox) {
  s_use_numeric_date = portalCheckboxChecked(numeric_checkbox);
  persistDateFormat();
}

bool applyAutoTimezone(const char* iana, const char* posix, double lat, double lon) {
  if (iana == nullptr || posix == nullptr || iana[0] == '\0' || posix[0] == '\0') {
    return false;
  }
  copyBlob(s_posix_tz, posix, sizeof(s_posix_tz));
  copyBlob(s_iana_name, iana, sizeof(s_iana_name));
  s_resolved_lat = lat;
  s_resolved_lon = lon;
  s_auto_timezone = true;
  persistAutoState();
  applyNtpConfig();
  Serial.printf("Clock: auto timezone %s (%s)\n", iana, posix);
  return true;
}

bool autoTimezoneMatchesCoords(double lat, double lon) {
  return s_auto_timezone && s_posix_tz[0] != '\0' &&
         coordsMatch(s_resolved_lat, s_resolved_lon, lat, lon);
}

const char* timezoneIanaName() { return s_iana_name; }

void formatTimezoneLabel(char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  if (s_auto_timezone) {
    if (s_iana_name[0] != '\0') {
      const char* slash = strrchr(s_iana_name, '/');
      const char* label = slash != nullptr ? slash + 1 : s_iana_name;
      char pretty[32];
      size_t pi = 0;
      for (size_t i = 0; label[i] != '\0' && pi + 1 < sizeof(pretty); ++i) {
        pretty[pi++] = label[i] == '_' ? ' ' : label[i];
      }
      pretty[pi] = '\0';
      snprintf(out, len, "Auto: %s", pretty);
      return;
    }
    snprintf(out, len, "Auto…");
    return;
  }
  char manual[16];
  formatManualTimezoneLabel(manual, sizeof(manual));
  snprintf(out, len, "%s", manual);
}

void formatTimeOfDay(char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  out[0] = '\0';
  if (!timeValid()) {
    strncpy(out, "--:--", len - 1);
    out[len - 1] = '\0';
    return;
  }

  struct tm local {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local);
  if (s_use_24h) {
    strftime(out, len, "%H:%M", &local);
  } else {
    strftime(out, len, "%I:%M", &local);
  }
  stripLeadingZeroHour(out);
}

void formatDateLine(char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  out[0] = '\0';
  if (!timeValid()) {
    strncpy(out, "Syncing time…", len - 1);
    out[len - 1] = '\0';
    return;
  }

  struct tm local {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local);
  formatCivilDate(local, s_use_numeric_date, out, len);
}

void formatClockFromEpoch(int64_t utc_epoch_sec, char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  out[0] = '\0';
  if (utc_epoch_sec < static_cast<int64_t>(kMinValidEpoch)) {
    strncpy(out, "--:--", len - 1);
    out[len - 1] = '\0';
    return;
  }
  const time_t utc = static_cast<time_t>(utc_epoch_sec);
  struct tm local {};
  localtime_r(&utc, &local);
  if (s_use_24h) {
    strftime(out, len, "%H:%M", &local);
  } else {
    strftime(out, len, "%I:%M %p", &local);
  }
  stripLeadingZeroHour(out);
}

void formatAmPm(char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  out[0] = '\0';
  if (s_use_24h || !timeValid()) {
    return;
  }

  struct tm local {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local);
  strftime(out, len, "%p", &local);
}

}  // namespace services::clock
