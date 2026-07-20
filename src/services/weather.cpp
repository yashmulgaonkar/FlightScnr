#include "services/weather.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstring>
#include <ctime>

#include "config.h"
#include "services/adsb_client.h"
#include "services/api_keys.h"
#include "services/clock_time.h"
#include "services/https_heap.h"
#include "services/https_lock.h"
#include "services/map_center.h"
#include "services/open_meteo_parse.h"

namespace services::weather {

namespace {

constexpr char kNs[] = "flightscnr";
constexpr char kImperialKey[] = "wx_imp";
constexpr uint32_t kTimeoutSec = config::kWeatherApiTimeoutMs / 1000UL;
constexpr time_t kMinValidEpoch = 1600000000;
/** Weather JSON must not live in internal DRAM — a full 1h+1d dump is ~30KB and
 *  truncates the Arduino String under heap pressure (IncompleteInput), then
 *  starves ADS-B (min free heap hundreds of bytes). */
constexpr size_t kWeatherMaxPayloadBytes = 96 * 1024;

bool s_imperial = config::kWeatherUseImperialDefault;

WeatherData s_live;
WeatherData s_staging;

// Weather borrows the ADS-B fetch worker (one TLS task for the whole device) so
// it never spawns a second TLS task — a second stack fragments the tight
// internal heap and starves the mbedTLS handshake.
volatile bool s_pending = false;  // queued on the worker or running
volatile bool s_ready = false;
unsigned long s_last_ok_ms = 0;
unsigned long s_last_attempt_ms = 0;
volatile unsigned long s_retry_after_ms = 0;
// Local calendar day (days since epoch in local time) of the last successful
// fetch, so the forecast is refreshed at midnight even within the staleness
// window. -1 = unknown / time not yet synced.
int32_t s_last_ok_local_day = -1;

// Days since the Unix epoch in local time, or -1 when the clock is unsynced.
int32_t currentLocalDayIndex() {
  return services::clock::localDayIndex();
}

double s_req_lat = 0.0;
double s_req_lon = 0.0;
bool s_req_imperial = false;
bool s_last_ok_location_valid = false;
double s_last_ok_lat = 0.0;
double s_last_ok_lon = 0.0;

bool locationMatches(double a_lat, double a_lon, double b_lat, double b_lon) {
  constexpr double kEps = 1e-5;
  return std::fabs(a_lat - b_lat) < kEps && std::fabs(a_lon - b_lon) < kEps;
}

bool cachedLocationStale() {
  if (!s_last_ok_location_valid || !s_live.valid) {
    return false;
  }
  return !locationMatches(s_last_ok_lat, s_last_ok_lon, services::map_center::latitude(),
                         services::map_center::longitude());
}

void noteSuccessfulFetchLocation() {
  s_last_ok_lat = s_req_lat;
  s_last_ok_lon = s_req_lon;
  s_last_ok_location_valid = true;
}

void workerYield() { vTaskDelay(1); }

// Days since 1970-01-01 for a civil date (proleptic Gregorian). Hinnant's algorithm.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Parse "YYYY-MM-DDTHH:MM:SS[Z]" (UTC) into an epoch in seconds. 0 on failure.
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

struct PsramPayload {
  char* data = nullptr;
  size_t len = 0;
  size_t cap = 0;

  ~PsramPayload() {
    if (data != nullptr) {
      heap_caps_free(data);
    }
  }

  bool reserve(size_t want) {
    if (want <= cap) {
      return true;
    }
    if (want > kWeatherMaxPayloadBytes) {
      return false;
    }
    void* p = heap_caps_realloc(data, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
      return false;
    }
    data = static_cast<char*>(p);
    cap = want;
    return true;
  }

  bool append(const uint8_t* src, size_t n) {
    if (len + n + 1 > cap) {
      size_t grow = cap < 4096 ? 4096 : cap * 2;
      if (grow < len + n + 1) {
        grow = len + n + 1;
      }
      if (!reserve(grow)) {
        return false;
      }
    }
    memcpy(data + len, src, n);
    len += n;
    data[len] = '\0';
    return true;
  }
};

struct WeatherJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  void deallocate(void* ptr) override {
    if (ptr != nullptr) {
      heap_caps_free(ptr);
    }
  }
  void* reallocate(void* ptr, size_t new_size) override {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};

WeatherJsonAllocator s_weather_json_allocator;

void readHttpPayload(HTTPClient& http, PsramPayload* payload, uint32_t timeout_ms) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr || payload == nullptr) {
    return;
  }
  const int content_len = http.getSize();
  // getSize() < 0 means the length is unknown — Tomorrow.io serves this body with
  // Transfer-Encoding: chunked, and getStreamPtr() hands us the RAW socket (chunk
  // size lines and all). We must de-chunk it ourselves; otherwise the payload
  // starts with a hex size like "79d7\r\n" and JSON parsing reads that as a bare
  // number and silently stops.
  const bool chunked = content_len < 0;
  if (content_len > 0 && !payload->reserve(static_cast<size_t>(content_len) + 1)) {
    Serial.printf("[weather] payload alloc failed (%d bytes)\n", content_len);
    return;
  }
  const unsigned long deadline_ms = millis() + timeout_ms;
  uint8_t buf[512];

  if (!chunked) {
    while (http.connected() || stream->available()) {
      if (millis() >= deadline_ms) {
        return;
      }
      if (stream->available() == 0) {
        if (!http.connected()) {
          break;
        }
        workerYield();
        continue;
      }
      const size_t got = stream->readBytes(buf, sizeof(buf));
      if (got == 0) {
        workerYield();
        continue;
      }
      if (!payload->append(buf, got)) {
        Serial.println("[weather] payload alloc failed (grow)");
        return;
      }
    }
    return;
  }

  // --- Chunked transfer decoding ---
  auto readByte = [&](uint8_t* out) -> bool {
    for (;;) {
      if (millis() >= deadline_ms) {
        return false;
      }
      if (stream->available() > 0) {
        const int c = stream->read();
        if (c >= 0) {
          *out = static_cast<uint8_t>(c);
          return true;
        }
      }
      if (!http.connected() && stream->available() == 0) {
        return false;
      }
      workerYield();
    }
  };

  for (;;) {
    // Read the chunk-size line (hex, possibly with ";extensions"); CRLF-terminated.
    char size_line[16] = {};
    size_t size_len = 0;
    uint8_t c = 0;
    while (readByte(&c)) {
      if (c == '\n') {
        break;
      }
      if (c != '\r' && size_len + 1 < sizeof(size_line)) {
        size_line[size_len++] = static_cast<char>(c);
      }
    }
    size_line[size_len] = '\0';
    const long chunk_size = strtol(size_line, nullptr, 16);
    if (chunk_size <= 0) {
      break;  // 0-size chunk terminates the body
    }
    long remaining = chunk_size;
    while (remaining > 0) {
      if (millis() >= deadline_ms) {
        return;
      }
      if (stream->available() == 0) {
        if (!http.connected()) {
          return;
        }
        workerYield();
        continue;
      }
      const size_t want = remaining < static_cast<long>(sizeof(buf))
                              ? static_cast<size_t>(remaining)
                              : sizeof(buf);
      const size_t got = stream->readBytes(buf, want);
      if (got == 0) {
        workerYield();
        continue;
      }
      if (!payload->append(buf, got)) {
        Serial.println("[weather] payload alloc failed (chunk grow)");
        return;
      }
      remaining -= static_cast<long>(got);
    }
    // Consume the CRLF that follows the chunk data.
    if (readByte(&c) && c == '\r') {
      readByte(&c);
    }
  }
}

// POST a JSON body to a Tomorrow.io endpoint over one TLS connection and
// deserialize the response. A single /v4/timelines POST returns BOTH the current
// conditions and the daily forecast, so one TLS handshake + one request covers
// the whole screen — critical on this heap-starved board (a second back-to-back
// handshake fails the mbedTLS SHA alloc) and it halves free-tier quota usage.
bool httpPostJson(HTTPClient& http, WiFiClientSecure& client, const String& url,
                  const String& body, JsonDocument* doc, const JsonDocument* filter,
                  int* out_code) {
  if (out_code != nullptr) {
    *out_code = 0;
  }
  if (!http.begin(client, url)) {
    Serial.println("[weather] http.begin failed");
    return false;
  }
  http.setConnectTimeout(config::kWeatherApiTimeoutMs);
  http.setTimeout(config::kWeatherApiTimeoutMs);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  const int code = http.POST(body);
  if (out_code != nullptr) {
    *out_code = code;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[weather] HTTP %d\n", code);
    // On an error status the body usually carries a JSON {"code":..,"message":..}
    // explaining why (bad key, bad field, quota). Dump it so failures aren't silent.
    PsramPayload err_body;
    readHttpPayload(http, &err_body, 2000U);
    if (err_body.len > 0) {
      Serial.printf("[weather] body: %.300s\n", err_body.data);
    }
    http.end();
    return false;
  }

  PsramPayload payload;
  readHttpPayload(http, &payload, config::kWeatherApiTimeoutMs + 4000U);
  http.end();
  workerYield();

  Serial.printf("[weather] HTTP %d len=%u\n", code, static_cast<unsigned>(payload.len));
  if (payload.len == 0 || payload.data == nullptr) {
    Serial.println("[weather] empty response body");
    return false;
  }
  const DeserializationError err =
      filter != nullptr
          ? deserializeJson(*doc, payload.data, payload.len,
                            DeserializationOption::Filter(*filter))
          : deserializeJson(*doc, payload.data, payload.len);
  if (err) {
    Serial.printf("[weather] JSON parse error: %s\n", err.c_str());
    Serial.printf("[weather] body: %.300s\n", payload.data);
    return false;
  }
  return true;
}

String unitsParam() { return s_req_imperial ? String("imperial") : String("metric"); }

String locationParam(double lat, double lon) {
  String loc = String(lat, 5);
  loc += ",";
  loc += String(lon, 5);
  return loc;
}

// One POST to /v4/timelines requesting current + daily. Avoid unbounded 1h
// timelines — those return ~30KB of hourly intervals, which used to truncate in
// an internal-heap String and yield IncompleteInput while nuking free RAM.
bool fetchTimelines(HTTPClient& http, WiFiClientSecure& client, double lat, double lon,
                    const char* key, WeatherData* out, int* out_code) {
  String url = "https://";
  url += config::kWeatherApiHost;
  url += "/v4/timelines?apikey=";
  url += key;

  String body = "{\"location\":\"";
  body += locationParam(lat, lon);
  body += "\",\"units\":\"";
  body += unitsParam();
  body +=
      "\",\"timesteps\":[\"current\",\"1d\"],\"startTime\":\"now\",\"endTime\":\"nowPlus3d\","
      "\"fields\":[\"temperature\",\"humidity\","
      "\"weatherCode\",\"temperatureMin\",\"temperatureMax\",\"weatherCodeMax\","
      "\"precipitationProbability\",\"sunriseTime\",\"sunsetTime\"]}";

  JsonDocument filter(&s_weather_json_allocator);
  JsonObject tl = filter["data"]["timelines"][0].to<JsonObject>();
  tl["timestep"] = true;
  JsonObject iv = tl["intervals"][0].to<JsonObject>();
  iv["startTime"] = true;
  JsonObject av = iv["values"].to<JsonObject>();
  av["temperature"] = true;
  av["humidity"] = true;
  av["weatherCode"] = true;
  av["temperatureMin"] = true;
  av["temperatureMax"] = true;
  av["weatherCodeMax"] = true;
  av["precipitationProbability"] = true;
  av["sunriseTime"] = true;
  av["sunsetTime"] = true;

  JsonDocument doc(&s_weather_json_allocator);
  if (!httpPostJson(http, client, url, body, &doc, &filter, out_code)) {
    return false;
  }

  bool got_current = false;
  bool got_daily = false;

  JsonArray timelines = doc["data"]["timelines"].as<JsonArray>();
  if (timelines.isNull()) {
    Serial.println("[weather] response had no data.timelines array");
    return false;
  }

  for (JsonObject tline : timelines) {
    const char* step = tline["timestep"].as<const char*>();
    JsonArray intervals = tline["intervals"].as<JsonArray>();
    if (step == nullptr || intervals.isNull() || intervals.size() == 0) {
      continue;
    }
    if (strcmp(step, "current") == 0 || strcmp(step, "1h") == 0) {
      JsonObject v = intervals[0]["values"].as<JsonObject>();
      if (!v.isNull()) {
        out->current_temp = v["temperature"].as<float>();
        out->current_humidity = v["humidity"].as<int>();
        out->current_code = v["weatherCode"].as<int>();
        got_current = true;
      }
    } else if (strcmp(step, "1d") == 0) {
      // Tomorrow.io daily buckets are anchored to ~6 AM local, so just after
      // midnight the first bucket still represents *yesterday*'s civil date.
      // Skip any leading bucket whose local date is before today so day 0 is
      // always today's date and we show today + the next 2 days.
      const int32_t today = currentLocalDayIndex();
      int out_idx = 0;
      for (int i = 0; i < static_cast<int>(intervals.size()) && out_idx < kForecastDays;
           ++i) {
        JsonObject d = intervals[i].as<JsonObject>();
        JsonObject v = d["values"].as<JsonObject>();
        if (v.isNull()) {
          continue;
        }
        const int64_t epoch = iso8601ToEpoch(d["startTime"].as<const char*>());
        if (today >= 0 && epoch > 0) {
          struct tm bucket_local {};
          const time_t bucket_utc = static_cast<time_t>(epoch);
          if (localtime_r(&bucket_utc, &bucket_local) != nullptr) {
            const int32_t bucket_day = static_cast<int32_t>(daysFromCivil(
                bucket_local.tm_year + 1900, static_cast<unsigned>(bucket_local.tm_mon + 1),
                static_cast<unsigned>(bucket_local.tm_mday)));
            if (bucket_day < today) {
              continue;  // stale pre-midnight bucket
            }
          }
        }
        DayForecast& fc = out->days[out_idx];
        fc.date_epoch = epoch;
        fc.temp_min = v["temperatureMin"].as<float>();
        fc.temp_max = v["temperatureMax"].as<float>();
        fc.weather_code = v["weatherCodeMax"].is<int>() ? v["weatherCodeMax"].as<int>()
                                                        : v["weatherCode"].as<int>();
        fc.precip_probability = v["precipitationProbability"].is<int>()
                                    ? v["precipitationProbability"].as<int>()
                                    : -1;
        fc.valid = true;
        if (out_idx == 0) {
          out->sunrise_epoch = iso8601ToEpoch(v["sunriseTime"].as<const char*>());
          out->sunset_epoch = iso8601ToEpoch(v["sunsetTime"].as<const char*>());
        }
        ++out_idx;
        got_daily = true;
      }
    }
  }

  if (!got_current && !got_daily) {
    Serial.println("[weather] parsed 0 usable timelines");
  }
  if (got_current || got_daily) {
    out->source = WeatherData::Source::TomorrowIo;
    return true;
  }
  return false;
}

// GET Open-Meteo forecast over one TLS connection and parse it. Key-less and
// no per-request throttling needed (free tier, ~10k/day). Requests current +
// 3-day daily in the active unit system with unixtime UTC epochs, then reuses
// the shared readHttpPayload + PSRAM buffer so it stays off the tight DRAM heap.
bool fetchOpenMeteo(HTTPClient& http, WiFiClientSecure& client, double lat, double lon,
                    WeatherData* out, int* out_code) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(lat, 5);
  url += "&longitude=";
  url += String(lon, 5);
  url += "&current=temperature_2m,relative_humidity_2m,weather_code";
  url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
         "precipitation_probability_max,sunrise,sunset";
  url += "&timezone=UTC&forecast_days=3&timeformat=unixtime&temperature_unit=";
  url += s_req_imperial ? "fahrenheit" : "celsius";

  if (out_code != nullptr) {
    *out_code = 0;
  }
  if (!http.begin(client, url)) {
    Serial.println("[weather] open-meteo http.begin failed");
    return false;
  }
  http.setConnectTimeout(config::kWeatherApiTimeoutMs);
  http.setTimeout(config::kWeatherApiTimeoutMs);
  http.addHeader("Accept", "application/json");

  const int code = http.GET();
  if (out_code != nullptr) {
    *out_code = code;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[weather] open-meteo HTTP %d\n", code);
    PsramPayload err_body;
    readHttpPayload(http, &err_body, 2000U);
    if (err_body.len > 0) {
      Serial.printf("[weather] body: %.300s\n", err_body.data);
    }
    http.end();
    return false;
  }

  PsramPayload payload;
  readHttpPayload(http, &payload, config::kWeatherApiTimeoutMs + 4000U);
  http.end();
  workerYield();

  Serial.printf("[weather] open-meteo HTTP %d len=%u\n", code,
                static_cast<unsigned>(payload.len));
  if (payload.len == 0 || payload.data == nullptr) {
    Serial.println("[weather] open-meteo empty response body");
    return false;
  }

  JsonDocument filter(&s_weather_json_allocator);
  buildOpenMeteoFilter(filter);
  JsonDocument doc(&s_weather_json_allocator);
  const DeserializationError err = deserializeJson(
      doc, payload.data, payload.len, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[weather] open-meteo JSON parse error: %s\n", err.c_str());
    Serial.printf("[weather] body: %.300s\n", payload.data);
    return false;
  }
  return parseOpenMeteo(doc, out);
}

// True when at least one weather provider is configured: paid Tomorrow.io (key +
// enabled) or the free key-less Open-Meteo fallback.
bool anyWeatherSourceAvailable() {
  return services::apikeys::canUseWeather() || services::apikeys::useOpenMeteo();
}

bool fetchWeatherBlocking(WeatherData* out) {
  if (!anyWeatherSourceAvailable()) {
    return false;
  }
  services::https::ScopedLock tls(config::kWeatherApiTimeoutMs * 2 + 4000);
  if (!tls.held()) {
    Serial.println("[weather] HTTPS busy (skipped)");
    return false;
  }

  // Let any prior ADS-B TLS session fully release its mbedTLS buffers and wait
  // for a healthy contiguous block before connecting — avoids -32512 churn.
  services::https::drainTlsHeapAfterSession();
  if (ESP.getFreeHeap() < config::kMinFreeHeapForWeather ||
      ESP.getMaxAllocHeap() < config::kMinContiguousHeapForWeather) {
    Serial.printf("[weather] defer: heap tight (free=%u max_blk=%u)\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    return false;
  }

  *out = WeatherData{};
  out->imperial = s_req_imperial;

  const char* key = services::apikeys::weatherKey();
  const bool tomorrow_ready =
      services::apikeys::canUseWeather() && key != nullptr && key[0] != '\0';

  const unsigned long started = millis();
  bool ok = false;
  int code = 0;
  {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(kTimeoutSec);
    client.setHandshakeTimeout(kTimeoutSec);
    HTTPClient http;
    // Paid Tomorrow.io first (one TLS handshake, one POST → current + daily).
    if (tomorrow_ready) {
      ok = fetchTimelines(http, client, s_req_lat, s_req_lon, key, out, &code);
      if (!ok) {
        // HTTP 429 = Tomorrow.io rate limit; back off much longer than a
        // transient failure so we stop hammering the free-tier quota.
        if (code == 429) {
          s_retry_after_ms = millis() + config::kWeatherRateLimitBackoffMs;
          Serial.printf("[weather] rate limited — backing off %lus\n",
                        config::kWeatherRateLimitBackoffMs / 1000UL);
        }
        client.stop();
        services::https::drainTlsHeapAfterSession();
      }
    }
    // Free key-less Open-Meteo when Tomorrow.io is off/keyless or just failed.
    if (!ok && services::apikeys::useOpenMeteo()) {
      if (tomorrow_ready) {
        // Discard any partial Tomorrow.io writes and reconnect for a clean
        // session after the failure.
        *out = WeatherData{};
        out->imperial = s_req_imperial;
        client.setInsecure();
        client.setTimeout(kTimeoutSec);
        client.setHandshakeTimeout(kTimeoutSec);
      }
      HTTPClient om_http;
      int om_code = 0;
      ok = fetchOpenMeteo(om_http, client, s_req_lat, s_req_lon, out, &om_code);
    }
    client.stop();
  }
  services::https::drainTlsHeapAfterSession();

  if (!ok) {
    return false;
  }
  out->valid = true;
  Serial.printf("[weather] ok src=%s temp=%.0f code=%d days=%d (%lums)\n",
                out->source == WeatherData::Source::OpenMeteo ? "open-meteo" : "tomorrow.io",
                out->current_temp, out->current_code,
                out->days[0].valid + out->days[1].valid + out->days[2].valid,
                millis() - started);
  return true;
}

// Runs on the ADS-B fetch worker (see services::adsb::queueBackgroundJob).
void weatherJob() {
  const unsigned long job_start = millis();
  if (config::kOvernightPerfLog) {
    Serial.printf("[weather] job start heap=%u max_blk=%u\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
  }
  WeatherData fresh{};
  if (fetchWeatherBlocking(&fresh)) {
    if (!locationMatches(s_req_lat, s_req_lon, services::map_center::latitude(),
                         services::map_center::longitude())) {
      Serial.println("[weather] discard fetch — map center moved");
    } else {
      fresh.fetched_ms = millis();
      s_staging = fresh;
      s_last_ok_ms = fresh.fetched_ms;
      s_last_ok_local_day = currentLocalDayIndex();
      noteSuccessfulFetchLocation();
      s_retry_after_ms = 0;
      s_ready = true;
    }
  } else {
    // Back off so a failure / heap-tight defer doesn't retry every loop. Don't
    // shorten a longer back-off already set (e.g. the 429 rate-limit window).
    const unsigned long base = millis() + config::kWeatherRetryBackoffMs;
    if (s_retry_after_ms < base) {
      s_retry_after_ms = base;
    }
    if (config::kOvernightPerfLog) {
      Serial.printf("[weather] job fail dur=%lums backoff=%lums heap=%u\n",
                    millis() - job_start, retryBackoffMs() / 1000UL, ESP.getFreeHeap());
    }
  }
  s_pending = false;
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (prefs.begin(kNs, true)) {
    s_imperial = prefs.getBool(kImperialKey, config::kWeatherUseImperialDefault);
    prefs.end();
  }
}

void init() {
  // Weather runs on the shared ADS-B worker; just make sure it exists.
  services::adsb::fetchInit();
}

void bootSanityCheck() {
  Serial.println("[weather] boot sanity check: one synchronous API call");
  if (!anyWeatherSourceAvailable()) {
    Serial.println("[weather] boot check: no weather source (Tomorrow.io off/keyless "
                   "and Open-Meteo off)");
    return;
  }
  s_req_lat = services::map_center::latitude();
  s_req_lon = services::map_center::longitude();
  s_req_imperial = s_imperial;
  Serial.printf("[weather] boot check: lat=%.5f lon=%.5f units=%s free=%u max_blk=%u\n",
                s_req_lat, s_req_lon, s_req_imperial ? "imperial" : "metric",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  WeatherData fresh{};
  if (!fetchWeatherBlocking(&fresh)) {
    Serial.println("[weather] boot check: FAILED (see HTTP/heap line above)");
    return;
  }
  fresh.fetched_ms = millis();
  s_staging = fresh;
  s_last_ok_ms = fresh.fetched_ms;
  s_last_ok_local_day = currentLocalDayIndex();
  noteSuccessfulFetchLocation();
  s_retry_after_ms = 0;
  s_ready = true;

  const char* u = fresh.imperial ? "F" : "C";
  Serial.printf("[weather] boot check: CURRENT temp=%.1f%s humidity=%d%% code=%d\n",
                fresh.current_temp, u, fresh.current_humidity, fresh.current_code);
  Serial.printf("[weather] boot check: sunrise=%ld sunset=%ld (UTC epoch)\n",
                static_cast<long>(fresh.sunrise_epoch),
                static_cast<long>(fresh.sunset_epoch));
  for (int i = 0; i < kForecastDays; ++i) {
    if (fresh.days[i].valid) {
      Serial.printf("[weather] boot check: day%d min=%.1f%s max=%.1f%s code=%d\n", i,
                    fresh.days[i].temp_min, u, fresh.days[i].temp_max, u,
                    fresh.days[i].weather_code);
    }
  }
}

bool useImperial() { return s_imperial; }

void setUseImperial(bool imperial) {
  if (imperial == s_imperial) {
    return;
  }
  s_imperial = imperial;
  Preferences prefs;
  if (prefs.begin(kNs, false)) {
    prefs.putBool(kImperialKey, s_imperial);
    prefs.end();
  }
  // Force a re-fetch in the new units the next time a screen asks.
  s_live.valid = false;
  s_retry_after_ms = 0;
  s_last_attempt_ms = 0;
}

bool saveUnitsFromForm(const char* units) {
  if (units == nullptr || units[0] == '\0') {
    return false;
  }
  bool imperial = s_imperial;
  if (units[0] == 'i' || units[0] == 'I' || units[0] == 'f' || units[0] == 'F') {
    imperial = true;
  } else if (units[0] == 'm' || units[0] == 'M' || units[0] == 'c' || units[0] == 'C') {
    imperial = false;
  } else {
    return false;
  }
  setUseImperial(imperial);
  return true;
}

void requestOnScreenOpen() {
  // Free the shared TLS worker for weather, then request. Do NOT clear the
  // back-off here: rapid screen switching must not bypass the 429 rate-limit
  // window or the per-attempt throttle.
  services::adsb::cancelPendingFetch();
  requestRefresh(!hasData() || cachedLocationStale());
}

void notifyLocationChanged() {
  if (!cachedLocationStale()) {
    return;
  }
  s_live.valid = false;
  s_ready = false;
  Serial.println("[weather] map center changed — refresh requested");
  services::adsb::cancelPendingFetch();
  requestRefresh(true);
}

void notifyApiKeyChanged() {
  if (!anyWeatherSourceAvailable()) {
    return;
  }
  s_retry_after_ms = 0;
  s_last_attempt_ms = 0;
  Serial.println("[weather] API key changed - refresh requested");
  services::adsb::cancelPendingFetch();
  requestRefresh(true);
}

void notifyEnabledChanged() {
  if (!anyWeatherSourceAvailable()) {
    s_live.valid = false;
    s_staging.valid = false;
    s_ready = false;
    Serial.println("[weather] no weather source - cache cleared");
    return;
  }
  notifyApiKeyChanged();
}

void requestRefresh(bool force) {
  if (!anyWeatherSourceAvailable()) {
    return;
  }
  if (s_pending) {
    return;
  }
  const unsigned long now = millis();
  // Back-off and anti-burst throttle are honored even on a forced refresh, so a
  // rate-limit window or a flurry of screen opens can't hammer the API.
  if (s_retry_after_ms != 0 && now < s_retry_after_ms) {
    return;
  }
  if (s_last_attempt_ms != 0 && (now - s_last_attempt_ms) < config::kWeatherMinFetchIntervalMs) {
    return;
  }
  // force only bypasses the staleness window (used for first load on screen open).
  // A local-day rollover (midnight) always counts as stale so the forecast's
  // "Today" column advances, even if the 30-minute window hasn't elapsed.
  const int32_t today = currentLocalDayIndex();
  const bool day_rolled = today >= 0 && s_last_ok_local_day >= 0 &&
                          today != s_last_ok_local_day;
  const bool location_changed = cachedLocationStale();
  if (!force && !day_rolled && !location_changed && s_live.valid &&
      s_live.imperial == s_imperial && s_last_ok_ms != 0 &&
      (now - s_last_ok_ms) < config::kWeatherStaleMs) {
    return;
  }
  s_req_lat = services::map_center::latitude();
  s_req_lon = services::map_center::longitude();
  s_req_imperial = s_imperial;
  // Borrow the ADS-B TLS worker; it runs weatherJob() when no ADS-B fetch is due.
  if (services::adsb::queueBackgroundJob(&weatherJob)) {
    s_last_attempt_ms = now;
    s_pending = true;
    if (config::kOvernightPerfLog) {
      Serial.printf("[weather] fetch queued force=%d day_roll=%d loc_chg=%d heap=%u\n",
                    force ? 1 : 0, day_rolled ? 1 : 0, location_changed ? 1 : 0,
                    ESP.getFreeHeap());
    }
  } else if (!hasData() &&
             (config::kSerialTraceDebug || config::kOvernightPerfLog)) {
    Serial.println("[weather] queue deferred (HTTPS worker busy)");
  }
}

bool fetchInProgress() { return s_pending; }

bool fetchReady() { return s_ready; }

bool processReady() {
  if (!s_ready) {
    return false;
  }
  s_live = s_staging;
  s_ready = false;
  return true;
}

bool hasData() { return s_live.valid; }

const WeatherData& data() { return s_live; }

uint32_t dataAgeMs() {
  if (s_last_ok_ms == 0 || !s_live.valid) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(millis() - s_last_ok_ms);
}

uint32_t retryBackoffMs() {
  if (s_retry_after_ms == 0) {
    return 0;
  }
  const unsigned long now = millis();
  if (now >= s_retry_after_ms) {
    return 0;
  }
  return static_cast<uint32_t>(s_retry_after_ms - now);
}

int currentIconCode() {
  if (!s_live.valid || s_live.current_code <= 0) {
    return 0;
  }
  bool night = false;
  const time_t now = time(nullptr);
  if (now >= kMinValidEpoch && s_live.sunrise_epoch > 0 && s_live.sunset_epoch > 0) {
    night = (now < s_live.sunrise_epoch) || (now > s_live.sunset_epoch);
  }
  return s_live.current_code * 10 + (night ? 1 : 0);
}

int dayIconCode(int index) {
  if (index < 0 || index >= kForecastDays || !s_live.days[index].valid) {
    return 0;
  }
  return s_live.days[index].weather_code * 10;
}

}  // namespace services::weather
