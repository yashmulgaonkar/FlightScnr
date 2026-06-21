#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

/** Vertical rate unavailable (baro_rate / geom_rate not in feed). */
constexpr int16_t kVertRateUnknown = INT16_MIN;

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  /** Airline + route ICAO codes (from adsb.fi feed and/or route API waterfall). */
  char airline[28];
  char route_origin[5];  /** Origin ICAO (e.g. KSFO). */
  char route_dest[5];    /** Destination ICAO (e.g. KBOS). */
  char type[5];
  char alt[12];
  /** Feet per minute from baro_rate (fallback geom_rate); kVertRateUnknown if missing. */
  int16_t vert_rate_fpm = kVertRateUnknown;
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Start background fetch worker (call once after WiFi is available). */
void fetchInit();

/** Queue a non-blocking fetch; returns false if a fetch is already running. */
bool fetchRequest(double center_lat, double center_lon, float fetch_radius_km);

/** True when a queued fetch finished and data is ready to display. */
bool fetchReady();

/** Publish staging to aircraftList(); optionally enrich routes first (skip on radar). */
void fetchProcessReady(bool enrich_routes = true);

/** Acknowledge fetchReady (call before reading aircraft for display). */
void fetchConsume();

/** Recover from a hung background fetch (call from main loop). */
void fetchWatchdog(unsigned long now_ms);

bool fetchInProgress();

/** Milliseconds since last successful ADS-B poll (UINT32_MAX if never). */
uint32_t lastFetchOkAgeMs();

/** Consecutive failed ADS-B polls since last success. */
uint32_t fetchFailStreak();

/** Free stack space (bytes) on the adsb_fetch FreeRTOS task. */
uint32_t fetchTaskStackFreeBytes();

void trafficFilterBootLoad();
int altitudeFloorFt();
void saveAltitudeFloorFromForm(const char* value);

}  // namespace services::adsb
