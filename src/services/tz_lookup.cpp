#include "services/tz_lookup.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "services/adsb_client.h"
#include "services/clock_time.h"
#include "services/https_heap.h"
#include "services/https_lock.h"
#include "services/map_center.h"

namespace services::tzlookup {

namespace {

volatile bool s_pending = false;
volatile bool s_resolved = false;
unsigned long s_last_attempt_ms = 0;
volatile unsigned long s_retry_after_ms = 0;

double s_req_lat = 0.0;
double s_req_lon = 0.0;

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t iso8601ToEpoch(const char* s) {
  if (s == nullptr) {
    return 0;
  }
  int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
  const int n = sscanf(s, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
  if (n < 3 || mon < 1 || mon > 12 || day < 1 || day > 31) {
    return 0;
  }
  const int64_t days = daysFromCivil(year, static_cast<unsigned>(mon),
                                     static_cast<unsigned>(day));
  return days * 86400 + hour * 3600 + min * 60 + sec;
}

int daysInMonth(int month, int year) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 30;
  }
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return kDays[month - 1];
}

void formatPosixOffsetPart(int32_t offset_sec, char* out, size_t len) {
  // timeapi: offset_sec added to UTC to get local civil time.
  // POSIX TZ uses west-positive hours after the abbreviation.
  const int32_t posix_sec = -offset_sec;
  const int32_t abs_sec = posix_sec >= 0 ? posix_sec : -posix_sec;
  const int h = abs_sec / 3600;
  const int m = (abs_sec % 3600) / 60;
  if (m == 0) {
    if (posix_sec < 0) {
      snprintf(out, len, "-%d", h);
    } else {
      snprintf(out, len, "%d", h);
    }
  } else if (posix_sec < 0) {
    snprintf(out, len, "-%d:%02d", h, m);
  } else {
    snprintf(out, len, "%d:%02d", h, m);
  }
}

bool buildTransitionRule(const char* iso, int32_t offset_sec, char* out, size_t len) {
  const int64_t epoch = iso8601ToEpoch(iso);
  if (epoch <= 0) {
    return false;
  }
  const time_t local_t = static_cast<time_t>(epoch + offset_sec);
  struct tm local {};
  if (gmtime_r(&local_t, &local) == nullptr) {
    return false;
  }
  const int month = local.tm_mon + 1;
  const int wday = local.tm_wday;
  const int mday = local.tm_mday;
  int week = (mday + 6) / 7;
  const int dim = daysInMonth(month, local.tm_year + 1900);
  if (mday + 7 > dim) {
    week = 5;
  }
  if (local.tm_min == 0 && local.tm_sec == 0) {
    snprintf(out, len, "M%d.%d.%d/%d", month, week, wday, local.tm_hour);
  } else {
    snprintf(out, len, "M%d.%d.%d/%d:%02d", month, week, wday, local.tm_hour, local.tm_min);
  }
  return true;
}

bool buildPosixTz(JsonObjectConst doc, char* out, size_t len) {
  JsonObjectConst std_off = doc["standardUtcOffset"];
  if (std_off.isNull()) {
    return false;
  }
  const int32_t std_sec = std_off["seconds"].as<int32_t>();

  char std_part[12];
  formatPosixOffsetPart(std_sec, std_part, sizeof(std_part));

  if (!doc["hasDayLightSaving"].as<bool>()) {
    snprintf(out, len, "UTC%s", std_part);
    return true;
  }

  JsonObjectConst dst = doc["dstInterval"];
  if (dst.isNull()) {
    snprintf(out, len, "UTC%s", std_part);
    return true;
  }

  const int32_t dst_delta = dst["dstOffsetToStandardTime"]["seconds"].as<int32_t>();
  char dst_part[12];
  formatPosixOffsetPart(std_sec + dst_delta, dst_part, sizeof(dst_part));

  const char* dst_start = dst["dstStart"];
  const char* dst_end = dst["dstEnd"];
  if (dst_start == nullptr || dst_end == nullptr) {
    snprintf(out, len, "UTC%s", std_part);
    return true;
  }

  char start_rule[24];
  char end_rule[24];
  if (!buildTransitionRule(dst_start, std_sec, start_rule, sizeof(start_rule)) ||
      !buildTransitionRule(dst_end, std_sec + dst_delta, end_rule, sizeof(end_rule))) {
    snprintf(out, len, "UTC%s", std_part);
    return true;
  }

  snprintf(out, len, "UTC%sUTC%s,%s,%s", std_part, dst_part, start_rule, end_rule);
  return true;
}

bool fetchTimezoneBlocking(double lat, double lon) {
  services::https::ScopedLock tls(config::kTzLookupTimeoutMs + 4000);
  if (!tls.held()) {
    Serial.println("[tz] HTTPS busy (lookup skipped)");
    return false;
  }

  String url = "https://";
  url += config::kTimeApiHost;
  url += "/api/TimeZone/coordinate?latitude=";
  url += String(lat, 5);
  url += "&longitude=";
  url += String(lon, 5);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(config::kTzLookupTimeoutMs / 1000UL);
  client.setHandshakeTimeout(config::kTzLookupTimeoutMs / 1000UL);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[tz] http.begin failed");
    return false;
  }
  http.setConnectTimeout(config::kTzLookupTimeoutMs);
  http.setTimeout(config::kTzLookupTimeoutMs);
  http.setReuse(false);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[tz] HTTP %d\n", code);
    http.end();
    client.stop();
    services::https::drainTlsHeapAfterSession();
    return false;
  }

  // getString() de-chunks transfer-encoded responses; timeapi.io serves this
  // body chunked, so reading the raw socket would leave hex size prefixes that
  // make ArduinoJson report InvalidInput.
  constexpr int kMaxTzPayloadBytes = 8192;
  const int content_len = http.getSize();
  if (content_len > kMaxTzPayloadBytes) {
    Serial.printf("[tz] response too large (Content-Length %d)\n", content_len);
    http.end();
    client.stop();
    services::https::drainTlsHeapAfterSession();
    return false;
  }
  String payload = http.getString();
  http.end();
  client.stop();
  services::https::drainTlsHeapAfterSession();
  if (payload.isEmpty()) {
    Serial.println("[tz] empty response");
    return false;
  }
  if (static_cast<int>(payload.length()) > kMaxTzPayloadBytes) {
    Serial.printf("[tz] response too large (%u bytes)\n",
                  static_cast<unsigned>(payload.length()));
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[tz] JSON parse failed: %s\n", err.c_str());
    return false;
  }

  const char* iana = doc["timeZone"].as<const char*>();
  if (iana == nullptr || iana[0] == '\0') {
    Serial.println("[tz] response missing timeZone");
    return false;
  }

  char posix[64];
  if (!buildPosixTz(doc.as<JsonObjectConst>(), posix, sizeof(posix))) {
    Serial.println("[tz] failed to build POSIX TZ string");
    return false;
  }

  if (!services::clock::applyAutoTimezone(iana, posix, lat, lon)) {
    return false;
  }

  Serial.printf("[tz] resolved %.5f,%.5f -> %s (%s)\n", lat, lon, iana, posix);
  return true;
}

void tzJob() {
  const double lat = s_req_lat;
  const double lon = s_req_lon;
  if (config::kOvernightPerfLog) {
    Serial.printf("[tz] job start lat=%.5f lon=%.5f heap=%u max_blk=%u\n", lat, lon,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  const bool ok = fetchTimezoneBlocking(lat, lon);
  s_pending = false;
  s_last_attempt_ms = millis();
  if (ok) {
    s_resolved = true;
    s_retry_after_ms = 0;
    if (config::kOvernightPerfLog) {
      Serial.printf("[tz] job ok heap=%u\n", ESP.getFreeHeap());
    }
  } else {
    s_resolved = services::clock::autoTimezoneMatchesCoords(lat, lon);
    s_retry_after_ms = millis() + config::kTzLookupRetryBackoffMs;
    if (config::kOvernightPerfLog) {
      Serial.printf("[tz] job fail resolved=%d backoff=%lums heap=%u\n", s_resolved ? 1 : 0,
                    config::kTzLookupRetryBackoffMs, ESP.getFreeHeap());
    }
  }
}

void queueLookup(double lat, double lon) {
  if (!services::clock::useAutoTimezone()) {
    return;
  }
  if (s_pending) {
    return;
  }
  const unsigned long now = millis();
  if (s_retry_after_ms != 0 && now < s_retry_after_ms) {
    return;
  }
  if (services::clock::autoTimezoneMatchesCoords(lat, lon)) {
    s_resolved = true;
    return;
  }

  s_req_lat = lat;
  s_req_lon = lon;
  if (services::adsb::queueBackgroundJob(&tzJob)) {
    s_pending = true;
    s_resolved = false;
    s_last_attempt_ms = now;
    Serial.printf("[tz] lookup queued lat=%.5f lon=%.5f\n", lat, lon);
  }
}

}  // namespace

void bootLoad() {
  s_pending = false;
  s_resolved = services::clock::autoTimezoneMatchesCoords(services::map_center::latitude(),
                                                          services::map_center::longitude());
}

void init() {
  services::adsb::fetchInit();
}

void requestForMapCenter() {
  queueLookup(services::map_center::latitude(), services::map_center::longitude());
}

void notifyLocationChanged() {
  s_resolved = false;
  s_retry_after_ms = 0;
  services::adsb::cancelPendingFetch();
  requestForMapCenter();
}

void tick() {
  if (!services::clock::useAutoTimezone()) {
    return;
  }
  if (s_pending) {
    return;
  }
  if (services::clock::autoTimezoneMatchesCoords(services::map_center::latitude(),
                                                 services::map_center::longitude())) {
    s_resolved = true;
    return;
  }
  queueLookup(services::map_center::latitude(), services::map_center::longitude());
}

bool lookupInProgress() { return s_pending; }

bool hasResolvedTimezone() {
  return s_resolved ||
         services::clock::autoTimezoneMatchesCoords(services::map_center::latitude(),
                                                    services::map_center::longitude());
}

}  // namespace services::tzlookup
