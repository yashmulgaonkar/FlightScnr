#pragma once

#include <cstdint>

namespace services::disclaimer {

/**
 * Bump when the on-screen disclaimer wording changes. A mismatched stored
 * version invalidates remembered acceptance so the user must Accept again.
 */
constexpr uint16_t kCurrentVersion = 1;

/** Load remembered version from NVS (call once at boot). */
void bootLoad();

/**
 * True when NVS holds both the current disclaimer text version and the
 * running firmware version. Every firmware update therefore requires a new
 * touchscreen acceptance.
 */
bool isRemembered();

/** Persist current disclaimer + firmware versions (touch Accept with checkbox on). */
void rememberCurrent();

/** Clear any stored acceptance (portal clear, or uncheck during countdown). */
void clear();

/** Stored version, or 0 when nothing is remembered. */
uint16_t storedVersion();

}  // namespace services::disclaimer
