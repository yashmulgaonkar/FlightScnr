#pragma once

#include <cstddef>
#include <cstdint>

class WebServer;

namespace services::route_cache {

/** One row in /route_cache.csv (matches route_lookup RAM slot). */
struct Entry {
  char callsign[9];
  char airline[28];
  char origin[5];
  char dest[5];
  uint8_t source;
  uint32_t cached_at_sec;
  bool api_done;
};

/** Mount LittleFS on the board's spiffs partition (no format). */
bool mount();

/** Scan flash for callsign; honor TTL for unresolved rows only. */
bool lookup(const char* callsign, Entry* out, uint32_t now_sec, uint32_t ttl_sec);

/** Scan flash for callsign; resolved or pending rows never expire (blocks repeat API use). */
bool lookupPermanent(const char* callsign, Entry* out);

void markDirty();

using ReadRamSlotFn = bool (*)(size_t index, Entry* out, size_t max_index);

/** Debounced merge-write of RAM + flash (call from main loop). */
void tick(unsigned long now_ms, ReadRamSlotFn read_slot, size_t max_slots, uint32_t now_sec,
          uint32_t ttl_sec);

/** Stream /route_cache.csv as a CSV download (settings web GET handler). */
void sendDownload(WebServer* server);

/** LittleFS usage for diagnostics (false if not mounted). */
bool flashUsage(size_t* used_bytes, size_t* total_bytes);

}  // namespace services::route_cache
