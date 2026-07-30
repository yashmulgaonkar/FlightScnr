#pragma once

#include <cstddef>
#include <cstdint>

namespace services::ota_github {

/** Load NVS cache (last check / latest tag / available). */
void init();

/**
 * If Wi‑Fi is up and 24h have passed since the last successful check (or never
 * checked), fetch GitHub releases/latest and update the cache. No-op when HTTPS
 * is busy or heap is tight. Safe to call from the main loop.
 */
void pollIfDue();

/**
 * Query GitHub for the latest release. When force is false, uses the daily
 * cache if still fresh. Returns true when the cache is valid after the call;
 * false on network/parse failure (previous cache retained).
 */
bool checkLatest(bool force);

/** True when cached latest tag is newer than the running firmware version. */
bool updateAvailable();

/** Cached latest release tag (e.g. "2026.7.30.1"), or empty. */
const char* latestTag();

/** Running firmware version string (same as config::kFirmwareVersion). */
const char* currentVersion();

enum class InstallState : uint8_t {
  Idle = 0,
  Running,
  Succeeded,
  Failed,
};

/** Start background install of the cached latest app.bin. False if busy/unavailable. */
bool startInstall();

InstallState installState();
/** 0–100 while Running; 100 on Succeeded. */
uint8_t installPercent();
/** Bytes written / expected total (0 if unknown) for progress UI. */
uint32_t installBytes();
uint32_t installTotal();
/** Short error text when Failed; otherwise empty. */
const char* installError();

}  // namespace services::ota_github
