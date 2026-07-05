#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

/** Vertical rate unavailable (baro_rate / geom_rate not in feed). */
constexpr int16_t kVertRateUnknown = INT16_MIN;

/** Aircraft classification derived from dbFlags, ADS-B category, and ICAO type. */
enum class AircraftCategory : uint8_t {
  Unknown = 0,
  Military,
  Heavy,
  Large,
  SmallAircraft,
  LightAircraft,
  Helicopter,
  UAV,
  Glider,
  LighterThanAir,
  HighPerformance,
  GroundVehicle,
  Private,
};

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  /** Airline + route ICAO codes (from adsb.fi feed and/or route API waterfall). */
  char airline[28];
  char airline_icao[4];  /** 3-letter ICAO operator code for logo lookup. */
  char route_origin[5];  /** Origin ICAO (e.g. KSFO). */
  char route_dest[5];    /** Destination ICAO (e.g. KBOS). */
  char type[5];
  char alt[12];
  char category[3];      /** ADS-B emitter category (e.g. "A3", "B6"). */
  char squawk[5];        /** Transponder squawk code (e.g. "7700", "1200"). */
  uint8_t db_flags = 0;  /** readsb dbFlags: bit0=military, bit1=interesting, bit2=PIA, bit3=LADD */
  /** Feet per minute from baro_rate (fallback geom_rate); kVertRateUnknown if missing. */
  int16_t vert_rate_fpm = kVertRateUnknown;

  bool isMilitary() const { return (db_flags & 0x01) != 0; }
  bool isInteresting() const { return (db_flags & 0x02) != 0; }
  /** 7700=emergency, 7600=radio failure, 7500=hijack. */
  bool isEmergencySquawk() const {
    return (squawk[0] == '7' && squawk[1] == '7' && squawk[2] == '0' && squawk[3] == '0') ||
           (squawk[0] == '7' && squawk[1] == '6' && squawk[2] == '0' && squawk[3] == '0') ||
           (squawk[0] == '7' && squawk[1] == '5' && squawk[2] == '0' && squawk[3] == '0');
  }

  /** Classify this aircraft based on all available data. */
  AircraftCategory classify() const;
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Thread-safe snapshot for UI (holds mutex briefly). Returns aircraft count copied. */
size_t copyAircraftSnapshot(Aircraft* dst, size_t max_count);

/** Copy one list slot under mutex; returns false if index out of range. */
bool copyAircraftAt(size_t index, Aircraft* dst);

/** Copy live aircraft by callsign under mutex. */
bool copyAircraftByCallsign(const char* callsign, Aircraft* dst);

/** Start background fetch worker (call once after WiFi is available). */
void fetchInit();

/** One-shot job run on the single TLS-owning fetch worker.
 *  Lets low-priority HTTPS users (e.g. weather) borrow this task instead of
 *  spawning a second TLS task — a second 12 KB stack fragments the tight
 *  internal heap and starves the mbedTLS handshake (esp-sha / -32512). The job
 *  runs only when no ADS-B fetch is pending (ADS-B has priority) and must
 *  acquire the HTTPS lock itself. */
using BackgroundJob = void (*)();
bool queueBackgroundJob(BackgroundJob job);
/** True while a queued background job is waiting or running. */
bool backgroundJobActive();

/** Drop a queued (not yet running) ADS-B fetch so the worker can run a
 *  background job (e.g. weather) immediately. In-flight TLS is not aborted. */
void cancelPendingFetch();

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

/** True while backing off after an adsb.fi HTTP 429 (rate limit). */
bool rateLimitBackoffActive(unsigned long now_ms);

/** Clear fail streak after a WiFi/TLS recycle. */
void fetchResetFailStreak();

/** Free stack space (bytes) on the adsb_fetch FreeRTOS task. */
uint32_t fetchTaskStackFreeBytes();

void trafficFilterBootLoad();
int altitudeFloorFt();
void saveAltitudeFloorFromForm(const char* value);

/** Merge route fields into the live aircraft list entry for callsign. */
void applyRouteFieldsByCallsign(const char* callsign, const char* airline,
                                const char* airline_icao, const char* origin,
                                const char* dest);

}  // namespace services::adsb
