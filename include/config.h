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

/** ADS-B poll interval (adsb.fi public limit ~1 req/s). */
constexpr unsigned long kTrafficPollIntervalMs = 2000;

/** ADS-B poll interval on flight detail when route enrichment is idle (ms). */
constexpr unsigned long kAdsbFetchPollIntervalDetailMs = 10000UL;

/** NTP servers (applied after Wi-Fi connect; timezone from clock settings). */
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";

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
constexpr bool kSerialTraceDebug = true;

/** Full ADS-B aircraft table on serial (very verbose). */
constexpr bool kAdsbVerboseAircraftLog = false;

/** Interval for [diag] serial diagnostics (ms). 0 = off. */
constexpr unsigned long kDiagLogIntervalMs = 60000UL;  // 5 min (overnight dev); 60000 = 1 min

/** Proactive WiFi/TLS refresh interval (0 = disabled). */
constexpr unsigned long kTlsProactiveRefreshMs = 4UL * 60UL * 60UL * 1000UL;

/** Reactive TLS recovery: recycle WiFi after this many consecutive ADS-B failures. */
constexpr uint32_t kTlsRecoverFailStreak = 3;
/** ...and no successful ADS-B fetch for at least this long (ms). */
constexpr uint32_t kTlsRecoverStaleMs = 15000UL;
/** Minimum time between proactive or reactive WiFi recycles (ms). */
constexpr unsigned long kTlsRecoverCooldownMs = 90000UL;

/** ADS-B poll interval while TLS failures are stacking (ms). */
constexpr unsigned long kAdsbFetchBackoffMs = 15000UL;

/** Defer ADS-B HTTPS if internal free heap is below this. */
constexpr uint32_t kMinFreeHeapForAdsbHttps = 28000;
/** ADS-B TLS + JSON — 16KB contiguous (max_blk often ~18KB after detail/web). */
constexpr uint32_t kMinContiguousHeapForAdsbTls = 16384;

/** Defer route API HTTPS if internal free heap is below this. */
constexpr uint32_t kMinFreeHeapForRouteHttps = 24000;
/** Route API TLS + JSON (detail screen shares heap with ADS-B TLS). */
constexpr uint32_t kMinContiguousHeapForRouteTls = 20480;

/** Route detail API connect/read timeout (ms). Keep short for fast scroll cancel. */
constexpr uint32_t kDetailApiTimeoutMs = 4000;
/** Restart route worker if stuck on a stale callsign (ms). */
constexpr unsigned long kDetailWorkerStallMs = 12000UL;
/** Faster recovery when the worker is enriching a callsign you scrolled past. */
constexpr unsigned long kDetailWorkerStaleStallMs = 6000UL;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
