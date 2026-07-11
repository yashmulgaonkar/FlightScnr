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
/** Minimum time radar stays visible before idle-clock can reclaim (ms).
 *  Gives ADS-B a chance to deliver aircraft data after screen opens. */
constexpr unsigned long kRadarMinVisibleMs = 5000;
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

/** [radar] resume diagnostics: detail->radar hang / full_draw_pending (disable once fixed). */
constexpr bool kRadarResumeDebug = true;

/** Full ADS-B aircraft table on serial (very verbose). */
constexpr bool kAdsbVerboseAircraftLog = false;

/** Interval for [diag] serial diagnostics (ms). 0 = off. */
constexpr unsigned long kDiagLogIntervalMs = 60000UL;  // 1 min

/** Expanded overnight performance logging: richer [diag], [perf], [nav], sweep gaps. */
constexpr bool kOvernightPerfLog = false;

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

/** Defer radar SPI panel writes when largest contiguous block is below this.
 *  The ESP-IDF SPI master driver uses internal DMA descriptors; corrupted driver
 *  state from prior heap exhaustion causes `spi_device_polling_end` assertion panics.
 *  Normal steady-state after TLS fragmentation is ~27636; only defer during true
 *  crisis when the driver's internal allocations would likely fail. */
constexpr uint32_t kMinContiguousHeapForPanelSpi = 10000;

/** Defer ADS-B HTTPS if internal free heap is below this.
 *  mbedTLS buffers (https_lock PSRAM allocator), the response payload, and the
 *  parsed JsonDocument all live in PSRAM now, so a fetch only needs internal RAM
 *  for lwIP socket buffers and small client structs (~2-4KB observed). Sized so
 *  the post-flight-detail steady state keeps polling: the lazily created
 *  route_detail worker task permanently carves its 16KB stack out of internal
 *  heap, settling free at ~26KB / max_blk ~12KB. The old 32000/18000 gates could
 *  never pass after that, deadlocking ADS-B ("[fetch] defer: heap low" forever). */
constexpr uint32_t kMinFreeHeapForAdsbHttps = 22000;
/** ADS-B TLS needs only small internal blocks now; panel SPI has its own guard. */
constexpr uint32_t kMinContiguousHeapForAdsbTls = 10000;

/** Defer route API HTTPS if internal free heap is below this.
 *  Slightly above the ADS-B bar so traffic polling wins when memory is tight. */
constexpr uint32_t kMinFreeHeapForRouteHttps = 24000;
/** Route API TLS + JSON — PSRAM-backed like ADS-B. */
constexpr uint32_t kMinContiguousHeapForRouteTls = 10000;
/** Target max_blk before opening another TLS session (after prior session ends). */
constexpr uint32_t kMinContiguousHeapForTlsReconnect = 20000;

/** Route detail API connect/read timeout (ms). Keep short for fast scroll cancel. */
constexpr uint32_t kDetailApiTimeoutMs = 4000;
/** Planespotters photo API + JPEG download timeout (ms). */
constexpr uint32_t kAircraftPhotoTimeoutMs = 5000;
/** Debounce before starting a photo fetch after encoder settle (ms). */
constexpr unsigned long kAircraftPhotoDebounceMs = 450UL;
/** Descriptive User-Agent required by Planespotters Photo API. */
constexpr char kPlanespottersUserAgent[] =
    "FlightScnr/" FLIGHTSCNR_FIRMWARE_VERSION
    " (+https://github.com/yashmulgaonkar/FlightScnr)";
/** Restart route worker if stuck on a stale callsign (ms). */
constexpr unsigned long kDetailWorkerStallMs = 12000UL;
/** Faster recovery when the worker is enriching a callsign you scrolled past. */
constexpr unsigned long kDetailWorkerStaleStallMs = 6000UL;
/** Longer flight-detail enrich debounce when max_blk is below pressure threshold. */
constexpr unsigned long kDetailEnrichDebouncePressureMs = 1200UL;
/** Enter enrich pressure mode (longer debounce, defer live APIs) below this max_blk.
 *  Must sit below the ~12KB max_blk that remains after the route_detail worker
 *  stack is carved out, or live route APIs never run on the detail screen. */
constexpr uint32_t kDetailEnrichHeapPressureBlk = 10000;

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
/** Weather is the lowest-priority HTTPS user — keep its bar above ADS-B so it
 *  defers when memory is tight. With mbedTLS in PSRAM the old 36000/20000 gates
 *  are obsolete and would permanently block weather after the route_detail
 *  worker stack claims its 16KB of internal heap. */
constexpr uint32_t kMinFreeHeapForWeather = 24000;
constexpr uint32_t kMinContiguousHeapForWeather = 12000;
/** Default unit system when the user has not overridden it (true = °F/imperial). */
constexpr bool kWeatherUseImperialDefault = false;

// --- Off-hours (night mode) ---
constexpr uint16_t kOffHoursDefaultStartMin = 1320;  // 22:00
constexpr uint16_t kOffHoursDefaultEndMin = 420;      // 07:00
constexpr unsigned long kOffHoursCheckIntervalMs = 60000;  // RTC poll every 60s

// --- FreeRTOS core affinity (ESP32-S3: 0 = PRO_CPU / WiFi, 1 = APP_CPU / UI) ---
/** HTTPS workers, WiFi event callbacks, mbedTLS — keep off the render loop core. */
constexpr uint8_t kCoreNetwork = 0;
/** Arduino loop(), display compositing, input — never block on TLS here. */
constexpr uint8_t kCoreUi = 1;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
