#pragma once

#include <cstddef>
#include <cstdint>

namespace services::weather {

constexpr int kForecastDays = 3;

struct DayForecast {
  bool valid = false;
  int64_t date_epoch = 0;  // start-of-day, UTC seconds
  int weather_code = 0;    // Tomorrow.io daily code (4-digit)
  float temp_min = 0.0f;   // in the active unit system
  float temp_max = 0.0f;
  int precip_probability = -1;  // percent; -1 = unavailable
};

struct WeatherData {
  bool valid = false;
  bool imperial = false;        // unit system this snapshot was fetched in
  float current_temp = 0.0f;
  int current_humidity = 0;     // percent
  int current_code = 0;         // Tomorrow.io realtime weather code (4-digit)
  int64_t sunrise_epoch = 0;    // UTC seconds (0 = unknown)
  int64_t sunset_epoch = 0;     // UTC seconds (0 = unknown)
  DayForecast days[kForecastDays];
  unsigned long fetched_ms = 0;
};

/** Load persisted unit preference. Call once at boot. */
void bootLoad();
/** Create the background fetch worker (idempotent). */
void init();

/** Diagnostic: synchronously make exactly ONE weather API call (on the calling
 *  task) and print the parsed result to serial. For use at boot to isolate the
 *  API/network path from the screen/worker scheduling. Caches the result. */
void bootSanityCheck();

bool useImperial();
void setUseImperial(bool imperial);
/** Web-form setter: accepts "imperial"/"metric"/"F"/"C"/"on"/"". */
bool saveUnitsFromForm(const char* units);

/** Kick a background fetch. With force=false it only fetches when the cache is
 *  missing, older than config::kWeatherStaleMs, the map center moved, or the
 *  local calendar day rolled over (and no fetch is already in flight). Safe to
 *  call every loop while a weather-bearing screen is open. */
void requestRefresh(bool force);

/** Call when the clock or forecast screen opens: cancel a queued ADS-B poll so
 * the shared TLS worker can run weather immediately, and force-fetch when we
 * have no cached data yet or the map center changed. */
void requestOnScreenOpen();

/** Call after the radar map center is saved to a new location. Invalidates
 *  weather cached for the previous coordinates and queues a refresh. */
void notifyLocationChanged();

/** Call after a weather API key is saved. Clears no-key backoff and queues a
 *  refresh so first-time setup produces data without waiting for a screen open. */
void notifyApiKeyChanged();

/** Call after the Tomorrow.io enable checkbox is saved. Clears cached data when
 *  disabled, or queues a refresh when re-enabled with a key. */
void notifyEnabledChanged();

bool fetchInProgress();
/** True when a background fetch has staged fresh data awaiting processReady(). */
bool fetchReady();
/** Promote staged data to live (call on the loop task). Returns true if applied. */
bool processReady();

bool hasData();
const WeatherData& data();
/** Milliseconds since the last successful fetch; UINT32_MAX when never. */
uint32_t dataAgeMs();
/** Milliseconds until a rate-limit retry is allowed; 0 when not backing off. */
uint32_t retryBackoffMs();

/** Full-day icon code for the current conditions (weatherCode*10, +1 at night). */
int currentIconCode();
/** Daytime icon code for a forecast day (weatherCode*10). */
int dayIconCode(int index);

}  // namespace services::weather
