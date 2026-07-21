#pragma once

#include <cstddef>

namespace services::alert {

/** Max stored registration length (Aircraft::registration is char[13]). */
constexpr size_t kWatchRegLen = 13;
constexpr size_t kWatchRegMax = 16;
constexpr size_t kWatchRegBlobLen = 160;

/**
 * Normalize a registration for storage: strip spaces, uppercase, keep hyphens.
 * Rejects empty / invalid. Returns false if not a plausible civil registration.
 */
bool normalizeWatchReg(const char* in, char* out, size_t out_len);

/** True for stored form (uppercase alnum + optional hyphens, length 3–12). */
bool isValidWatchReg(const char* reg);

/**
 * Parse comma-separated registrations into dest[][]; returns count written.
 * Invalid tokens skipped; duplicates ignored (hyphen-insensitive).
 */
size_t parseWatchRegBlob(const char* blob, char dest[][kWatchRegLen], size_t max_entries);

void rebuildWatchRegBlob(const char entries[][kWatchRegLen], size_t count, char* out,
                         size_t out_len);

/** Match list entry against ADS-B registration (hyphen/space-insensitive). */
bool watchRegListContains(const char entries[][kWatchRegLen], size_t count,
                          const char* registration);

}  // namespace services::alert
