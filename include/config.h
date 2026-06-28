#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

#ifndef FLIGHTSCNR_FIRMWARE_VERSION
#define FLIGHTSCNR_FIRMWARE_VERSION "dev"
#endif

/** Build-time firmware version (release CI sets FLIGHTSCNR_FIRMWARE_VERSION). */
constexpr char kFirmwareVersion[] = FLIGHTSCNR_FIRMWARE_VERSION;

/** Project source repository (linked on device web UIs). */
constexpr char kGithubRepoUrl[] =
    "https://github.com/yashmulgaonkar/FlightScnr";

/** Captive portal footer link (placeholder until author page is ready). */
constexpr char kPortalAuthorUrl[] = "https://github.com/yashmulgaonkar/FlightScnr";

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "FlightScnr-AP";
constexpr char kPortalIp[] = "4.3.2.1";
/** mDNS host (no ".local" suffix); browser: http://flightscnr.local */
constexpr char kPortalHostname[] = "flightscnr";
constexpr char kPortalHostUrl[] = "flightscnr.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- Knob button (GPIO 0, active LOW) ---
constexpr gpio_num_t kKnobKeyPin = GPIO_NUM_0;
constexpr unsigned long kKnobResetHoldMs = 3000UL;
/** Ignore knob taps shorter than this (debounce). */
constexpr unsigned long kKnobTapMinMs = 40UL;
/** Ignore swipe gestures briefly after a screen change (prevents one gesture
 *  from triggering two transitions, e.g. clock→radar then radar→details). */
constexpr unsigned long kSwipeNavDebounceMs = 300UL;

// --- Rotary encoder ---
constexpr gpio_num_t kKnobPinA = GPIO_NUM_1;
constexpr gpio_num_t kKnobPinB = GPIO_NUM_2;

// --- Display: 1.2" round 390×390 AMOLED QSPI (SH8601 or CO5300, auto-detected) ---
constexpr int kDisplayWidth = 390;
constexpr int kDisplayHeight = 390;

/** Flight detail / device settings return to radar; clock settings return to clock (ms). */
constexpr unsigned long kSecondaryScreenTimeoutMs = 10000;
/** Details splash shown at boot before radar (ms). */
constexpr unsigned long kBootDetailsDurationMs = 5000;

// --- Map center factory defaults (portal can override) ---
constexpr double kFactoryLatitude = 37.61977;
constexpr double kFactoryLongitude = -122.37227;

/** ADS-B poll interval (adsb.fi public limit ~1 req/s).
 *  The fetch worker is pinned to core 0 and the render loop is decoupled, so a
 *  faster poll no longer stutters the sweep. Effective blip refresh is
 *  max(this, fetch duration); fetches take ~3-8s due to a fresh TLS handshake
 *  per request, so 2s keeps us well under the ~1 req/s API limit. */
constexpr unsigned long kTrafficPollIntervalMs = 2000;

/** ADS-B poll interval on flight detail when route enrichment is idle (ms). */
constexpr unsigned long kAdsbFetchPollIntervalDetailMs = 10000UL;

/** NTP servers (applied after Wi-Fi connect; timezone from clock settings). */
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";

/** timeapi.io host for lat/lon -> IANA timezone lookup (HTTPS, no API key). */
constexpr char kTimeApiHost[] = "timeapi.io";
constexpr uint32_t kTzLookupTimeoutMs = 8000;
constexpr unsigned long kTzLookupRetryBackoffMs = 60000UL;

/** Route API limit defaults (monthly; reset on calendar month when time is synced). */
/** AirLabs free tier: up to 1,000 queries/month. */
constexpr uint32_t kDefaultAirLabsMaxCalls = 1000;
constexpr uint32_t kDefaultFlightAwareBudgetUsdMicro = 5000000UL;  // $5.00 (Personal free tier)
/** GET /flights/{ident} — $0.005/result set per FlightAware AeroAPI pricing. */
constexpr uint32_t kDefaultFlightAwareCostUsdMicro = 5000UL;       // $0.005
constexpr uint32_t kDefaultFr24BudgetUsdMicro = 9000000UL;         // $9.00
constexpr uint32_t kDefaultFr24CostUsdMicro = 300UL;             // $0.0003
constexpr unsigned kRouteLookupCacheTtlSec = 3600;
/** Debounced write of /route_cache.csv to the ~3.4 MB LittleFS partition. */
constexpr unsigned long kRouteCacheFlushIntervalMs = 600000UL;
constexpr size_t kRouteCacheFileMaxEntries = 1500;
/** Include aircraft reporting ground baro altitude. */
constexpr bool kTrafficIncludeGround = false;
/** Minimum altitude (ft); 0 disables the filter. */
constexpr int kFactoryAltitudeFloorFt = 500;

/** Serial [gfx]/[radar] SPI debug (set false once stable). */
constexpr bool kGfxDebug = false;

/** Serial [detail]/[fetch] trace for scroll, draw, enrich, TLS (disable once stable). */
constexpr bool kSerialTraceDebug = false;

/** Serial [sweep] radar animation / blit stall diagnostics (disable once stable).
 *  Off by default: the per-frame Serial.printf costs ~5ms of a 33ms frame budget
 *  and, under HTTPS load, dropped TX lines make the angle log look like it jumps
 *  ~35° even though the sweep actually rendered every intermediate frame. */
constexpr bool kRadarSweepTraceDebug = false;

/** Full ADS-B aircraft table on serial (very verbose). */
constexpr bool kAdsbVerboseAircraftLog = false;

/** Interval for [diag] serial diagnostics (ms). 0 = off. */
constexpr unsigned long kDiagLogIntervalMs = 60000UL;  // 1 min

/** Expanded overnight performance logging: richer [diag], [perf], [nav], sweep gaps. */
constexpr bool kOvernightPerfLog = true;

/** Log [perf] slow_loop when loop() exceeds this (ms). 0 = off. */
constexpr unsigned long kDiagSlowLoopMs = 75UL;

/** Log [perf] sweep_gap when inter-frame gap exceeds kSweepFrameMs * this. 0 = off. */
constexpr unsigned kDiagSweepGapFrameMult = 3;

/** Proactive WiFi/TLS refresh interval (0 = disabled). */
constexpr unsigned long kTlsProactiveRefreshMs = 4UL * 60UL * 60UL * 1000UL;

/** Reactive TLS recovery: recycle WiFi after this many consecutive ADS-B failures. */
constexpr uint32_t kTlsRecoverFailStreak = 3;
/** ...and no successful ADS-B fetch for at least this long (ms). */
constexpr uint32_t kTlsRecoverStaleMs = 15000UL;
/** Minimum time between proactive or reactive WiFi recycles (ms). */
constexpr unsigned long kTlsRecoverCooldownMs = 90000UL;
/** Shorter cooldown after mbedTLS memory allocation failures (ms). */
constexpr unsigned long kTlsMemoryRecoverCooldownMs = 15000UL;

/** ADS-B poll interval while TLS failures are stacking (ms). */
constexpr unsigned long kAdsbFetchBackoffMs = 15000UL;

/** Pause ADS-B polling this long after an adsb.fi HTTP 429 (rate limit).
 *  A 429 is throttling, not a TLS fault, so we back off instead of recycling
 *  WiFi (which would reconnect and immediately hammer the API again). */
constexpr unsigned long kAdsbRateLimitBackoffMs = 15000UL;

/** Defer ADS-B HTTPS if internal free heap is below this.
 *  Raised from 28000: a large response (13+ aircraft) consumed ~27KB of internal
 *  heap mid-fetch and drove `min` to 628 bytes, corrupting the SPI driver state
 *  (spi_device_polling_end panic). This floor keeps ~12KB headroom after the
 *  peak so an allocation can never return null underneath the display driver. */
constexpr uint32_t kMinFreeHeapForAdsbHttps = 42000;
/** ADS-B TLS + JSON — require a healthy contiguous block too. */
constexpr uint32_t kMinContiguousHeapForAdsbTls = 20000;

/** Defer route API HTTPS if internal free heap is below this.
 *  Raised from 24000 for the same reason: route enrichment polls run on the
 *  flight-detail screen alongside the full ADS-B response in memory. */
constexpr uint32_t kMinFreeHeapForRouteHttps = 38000;
/** Route API TLS + JSON — require a healthy contiguous block too. */
constexpr uint32_t kMinContiguousHeapForRouteTls = 18000;
/** Target max_blk before opening another TLS session (after prior session ends). */
constexpr uint32_t kMinContiguousHeapForTlsReconnect = 40000;

/** Route detail API connect/read timeout (ms). Keep short for fast scroll cancel. */
constexpr uint32_t kDetailApiTimeoutMs = 4000;
/** Restart route worker if stuck on a stale callsign (ms). */
constexpr unsigned long kDetailWorkerStallMs = 12000UL;
/** Faster recovery when the worker is enriching a callsign you scrolled past. */
constexpr unsigned long kDetailWorkerStaleStallMs = 6000UL;

// --- Weather (Tomorrow.io) ---
/** Tomorrow.io API host (HTTPS). One realtime + one daily-forecast GET per refresh. */
constexpr char kWeatherApiHost[] = "api.tomorrow.io";
/** Refresh weather at most this often while the clock screen is left on (ms). */
constexpr unsigned long kWeatherClockRefreshMs = 30UL * 60UL * 1000UL;  // 30 min
/** Re-fetch on screen open only if the cache is older than this (ms). */
constexpr unsigned long kWeatherStaleMs = 30UL * 60UL * 1000UL;  // 30 min
/** Connect/read timeout for a weather request (ms). */
constexpr uint32_t kWeatherApiTimeoutMs = 6000;
/** After a failed weather fetch, wait this long before retrying (ms).
 *  Weather is low priority and shares TLS/heap with ADS-B, so failures back off
 *  instead of hammering connects (which fragments the heap and spams -32512). */
constexpr unsigned long kWeatherRetryBackoffMs = 60000UL;
/** Longer back-off after an HTTP 429 (Tomorrow.io free tier ~25 req/hour). */
constexpr unsigned long kWeatherRateLimitBackoffMs = 5UL * 60UL * 1000UL;  // 5 min
/** Minimum spacing between weather fetch attempts — throttles rapid screen
 *  switching so a burst of opens can't blow the API's per-second limit. */
constexpr unsigned long kWeatherMinFetchIntervalMs = 20000UL;  // 20 s
/** Weather is the lowest-priority HTTPS user, so it only opens a TLS session
 *  when there is real headroom — a marginal heap fails the mbedTLS SHA buffer
 *  allocation (esp-sha "Failed to allocate buf memory" / -32512). Higher than
 *  the ADS-B bar so weather defers to the clock idling instead of contending. */
constexpr uint32_t kMinFreeHeapForWeather = 36000;
constexpr uint32_t kMinContiguousHeapForWeather = 20000;
/** Default unit system when the user has not overridden it (true = °F/imperial). */
constexpr bool kWeatherUseImperialDefault = false;

// --- FreeRTOS core affinity (ESP32-S3: 0 = PRO_CPU / WiFi, 1 = APP_CPU / UI) ---
/** HTTPS workers, WiFi event callbacks, mbedTLS — keep off the render loop core. */
constexpr uint8_t kCoreNetwork = 0;
/** Arduino loop(), display compositing, input — never block on TLS here. */
constexpr uint8_t kCoreUi = 1;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
