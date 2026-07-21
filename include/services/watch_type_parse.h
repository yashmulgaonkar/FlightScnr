#pragma once

#include <cstddef>

namespace services::alert {

/** Max ICAO type designator length (Aircraft::type is char[5]). */
constexpr size_t kWatchTypeLen = 5;
constexpr size_t kWatchTypeMax = 16;
constexpr size_t kWatchTypeBlobLen = 96;

/**
 * Normalize an ICAO type token: strip spaces, uppercase, cap at 4 chars.
 * Rejects empty / non-alnum. Returns false if the result is not a valid watch type.
 */
bool normalizeWatchType(const char* in, char* out, size_t out_len);

/** True for 2–4 alphanumeric ICAO type designators (e.g. B738, A333, C2). */
bool isValidWatchType(const char* type);

/**
 * Parse comma-separated ICAO types into dest[][]; returns count written (≤ max_entries).
 * Invalid tokens are skipped. Duplicates are ignored.
 */
size_t parseWatchTypeBlob(const char* blob, char dest[][kWatchTypeLen], size_t max_entries);

/** Rebuild comma-separated blob from entries. Truncates if out_len is too small. */
void rebuildWatchTypeBlob(const char entries[][kWatchTypeLen], size_t count, char* out,
                          size_t out_len);

/** True if normalized type is in the list. */
bool watchTypeListContains(const char entries[][kWatchTypeLen], size_t count,
                           const char* type);

}  // namespace services::alert
