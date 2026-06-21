#include "services/route_cache_store.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <WebServer.h>

#include <freertos/semphr.h>

#include <cstring>

#include "config.h"

namespace services::route_cache {

namespace {

constexpr char kPath[] = "/route_cache.csv";
constexpr char kTmpPath[] = "/route_cache.tmp";
constexpr char kHeader[] = "callsign,airline,origin,dest,source,cached_unix,api_done\n";

enum class MountState : uint8_t { kPending, kOk, kFailed };

MountState s_mount = MountState::kPending;
bool s_dirty = false;
unsigned long s_last_flush_ms = 0;
SemaphoreHandle_t s_fs_mutex = nullptr;

/** Merge buffer for flush — must not live on loop task stack (~84 KB at 1500 rows). */
Entry s_merged[config::kRouteCacheFileMaxEntries];

void ensureFsMutex() {
  if (s_fs_mutex == nullptr) {
    s_fs_mutex = xSemaphoreCreateMutex();
  }
}

struct FsLock {
  bool acquire(TickType_t timeout) {
    ensureFsMutex();
    if (s_fs_mutex == nullptr) {
      return false;
    }
    held_ = xSemaphoreTake(s_fs_mutex, timeout) == pdTRUE;
    return held_;
  }

  ~FsLock() {
    if (held_ && s_fs_mutex != nullptr) {
      xSemaphoreGive(s_fs_mutex);
    }
  }

  bool held_ = false;
};

void noteFlushAttempt(unsigned long now_ms) {
  s_last_flush_ms = now_ms;
}

void ensureCacheFile() {
  if (!LittleFS.exists(kPath)) {
    File created = LittleFS.open(kPath, "w");
    if (created) {
      created.print(kHeader);
      created.close();
    }
  }
}

bool ensureMounted() {
  if (s_mount == MountState::kOk) {
    return true;
  }
  if (s_mount == MountState::kFailed) {
    return false;
  }

  if (LittleFS.begin(false)) {
    s_mount = MountState::kOk;
    ensureCacheFile();
    return true;
  }

  Serial.println("route_cache: partition not LittleFS — formatting spiffs slice...");
  if (!LittleFS.format()) {
    Serial.println("route_cache: format failed");
    s_mount = MountState::kFailed;
    return false;
  }
  if (!LittleFS.begin(false)) {
    Serial.println("route_cache: mount failed after format");
    s_mount = MountState::kFailed;
    return false;
  }

  Serial.println("route_cache: LittleFS ready");
  s_mount = MountState::kOk;
  ensureCacheFile();
  return true;
}

bool entryResolved(const Entry& e) {
  const bool has_data =
      e.airline[0] != '\0' || e.origin[0] != '\0' || e.dest[0] != '\0';
  return e.api_done || has_data || e.source == 5;  // ApiSource::kPrefix
}

bool entryPending(const Entry& e) {
  return e.callsign[0] != '\0' && !e.api_done && !entryResolved(e);
}

bool entryExpired(const Entry& e, uint32_t now_sec, uint32_t ttl_sec) {
  if (e.cached_at_sec == 0) {
    return true;
  }
  return (now_sec - e.cached_at_sec) > ttl_sec;
}

bool entryKeepInFlash(const Entry& e, uint32_t now_sec, uint32_t ttl_sec) {
  if (entryResolved(e) || entryPending(e)) {
    return true;
  }
  return !entryExpired(e, now_sec, ttl_sec);
}

/** Trim trailing CR/LF/spaces in place. */
void trimLine(char* line) {
  if (line == nullptr) {
    return;
  }
  size_t n = strlen(line);
  while (n > 0 &&
         (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
    line[n - 1] = '\0';
    --n;
  }
}

const char* nextField(char* line, char** rest) {
  char* comma = strchr(line, ',');
  if (comma != nullptr) {
    *comma = '\0';
    *rest = comma + 1;
  } else {
    *rest = nullptr;
  }
  return line;
}

bool parseCsvLine(char* line, Entry* out) {
  if (out == nullptr || line == nullptr) {
    return false;
  }
  trimLine(line);
  if (line[0] == '\0' || line[0] == '#') {
    return false;
  }
  if (strncmp(line, "callsign", 8) == 0) {
    return false;
  }

  char* rest = line;
  char* field = rest;
  nextField(rest, &rest);
  if (field[0] == '\0') {
    return false;
  }
  strncpy(out->callsign, field, sizeof(out->callsign) - 1);
  out->callsign[sizeof(out->callsign) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  strncpy(out->airline, field, sizeof(out->airline) - 1);
  out->airline[sizeof(out->airline) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  strncpy(out->origin, field, sizeof(out->origin) - 1);
  out->origin[sizeof(out->origin) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  strncpy(out->dest, field, sizeof(out->dest) - 1);
  out->dest[sizeof(out->dest) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  out->source = 0;
  if (strcmp(field, "AL") == 0) {
    out->source = 2;
  } else if (strcmp(field, "FA") == 0) {
    out->source = 3;
  } else if (strcmp(field, "FR") == 0) {
    out->source = 4;
  } else if (strcmp(field, "pfx") == 0) {
    out->source = 5;
  } else if (strcmp(field, "cache") == 0) {
    out->source = 1;
  }

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  out->cached_at_sec = static_cast<uint32_t>(strtoul(field, nullptr, 10));

  field = rest;
  if (field == nullptr) {
    return false;
  }
  out->api_done = (strcmp(field, "1") == 0 || strcmp(field, "true") == 0);
  return true;
}

void formatSourceTag(uint8_t source, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  const char* tag = "";
  switch (source) {
    case 2:
      tag = "AL";
      break;
    case 3:
      tag = "FA";
      break;
    case 4:
      tag = "FR";
      break;
    case 5:
      tag = "pfx";
      break;
    case 1:
      tag = "cache";
      break;
    default:
      break;
  }
  strncpy(out, tag, out_len - 1);
  out[out_len - 1] = '\0';
}

int findMergedIndex(const Entry* entries, size_t count, const char* callsign) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(entries[i].callsign, callsign) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void upsertMerged(Entry* entries, size_t* count, const Entry& e) {
  const int idx = findMergedIndex(entries, *count, e.callsign);
  if (idx >= 0) {
    entries[idx] = e;
    return;
  }
  if (*count >= config::kRouteCacheFileMaxEntries) {
    size_t oldest = 0;
    for (size_t i = 1; i < *count; ++i) {
      if (entries[i].cached_at_sec < entries[oldest].cached_at_sec) {
        oldest = i;
      }
    }
    entries[oldest] = e;
    return;
  }
  entries[*count] = e;
  ++(*count);
}

bool writeEntry(File& f, const Entry& e) {
  char src[8];
  formatSourceTag(e.source, src, sizeof(src));
  return f.printf("%s,%s,%s,%s,%s,%lu,%u\n", e.callsign,
                  e.airline[0] != '\0' ? e.airline : "", e.origin[0] != '\0' ? e.origin : "",
                  e.dest[0] != '\0' ? e.dest : "", src,
                  static_cast<unsigned long>(e.cached_at_sec),
                  e.api_done ? 1u : 0u) > 0;
}

}  // namespace

bool mount() { return ensureMounted(); }

bool lookup(const char* callsign, Entry* out, uint32_t now_sec, uint32_t ttl_sec) {
  if (out == nullptr || callsign == nullptr || callsign[0] == '\0') {
    return false;
  }
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(2000))) {
    return false;
  }
  if (!ensureMounted() || !LittleFS.exists(kPath)) {
    return false;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    return false;
  }

  char line[160];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n == 0) {
      continue;
    }
    line[n] = '\0';

    Entry parsed;
    if (!parseCsvLine(line, &parsed)) {
      continue;
    }
    if (strcmp(parsed.callsign, callsign) != 0) {
      continue;
    }
    if (entryExpired(parsed, now_sec, ttl_sec)) {
      continue;
    }
    if (!entryResolved(parsed)) {
      continue;
    }
    *out = parsed;
    f.close();
    return true;
  }

  f.close();
  return false;
}

bool lookupPermanent(const char* callsign, Entry* out) {
  if (out == nullptr || callsign == nullptr || callsign[0] == '\0') {
    return false;
  }
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(2000))) {
    return false;
  }
  if (!ensureMounted() || !LittleFS.exists(kPath)) {
    return false;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    return false;
  }

  char line[160];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n == 0) {
      continue;
    }
    line[n] = '\0';

    Entry parsed;
    if (!parseCsvLine(line, &parsed)) {
      continue;
    }
    if (strcmp(parsed.callsign, callsign) != 0) {
      continue;
    }
    if (!entryResolved(parsed) && !entryPending(parsed)) {
      continue;
    }
    *out = parsed;
    f.close();
    return true;
  }

  f.close();
  return false;
}

void markDirty() { s_dirty = true; }

void tick(unsigned long now_ms, ReadRamSlotFn read_slot, size_t max_slots,
          uint32_t now_sec, uint32_t ttl_sec) {
  if (!s_dirty) {
    return;
  }

  if (s_last_flush_ms == 0) {
    s_last_flush_ms = now_ms;
    return;
  }
  if ((now_ms - s_last_flush_ms) < config::kRouteCacheFlushIntervalMs) {
    return;
  }

  FsLock lock;
  if (!lock.acquire(0)) {
    return;
  }

  if (read_slot == nullptr || !ensureMounted()) {
    return;
  }

  size_t merged_count = 0;

  for (size_t i = 0; i < max_slots; ++i) {
    Entry slot;
    if (!read_slot(i, &slot, max_slots) || slot.callsign[0] == '\0') {
      continue;
    }
    if (!entryKeepInFlash(slot, now_sec, ttl_sec)) {
      continue;
    }
    upsertMerged(s_merged, &merged_count, slot);
  }

  if (LittleFS.exists(kPath)) {
  File fin = LittleFS.open(kPath, "r");
  if (fin) {
    char line[160];
    while (fin.available()) {
      const size_t n = fin.readBytesUntil('\n', line, sizeof(line) - 1);
      if (n == 0) {
        continue;
      }
      line[n] = '\0';
      Entry parsed;
      if (!parseCsvLine(line, &parsed)) {
        continue;
      }
      if (!entryKeepInFlash(parsed, now_sec, ttl_sec)) {
        continue;
      }
      if (findMergedIndex(s_merged, merged_count, parsed.callsign) >= 0) {
        continue;
      }
      upsertMerged(s_merged, &merged_count, parsed);
    }
    fin.close();
  }
  }

  File fout = LittleFS.open(kTmpPath, "w");
  if (!fout) {
    Serial.println("[route_cache] write open failed");
    noteFlushAttempt(now_ms);
    return;
  }
  fout.print(kHeader);
  for (size_t i = 0; i < merged_count; ++i) {
    writeEntry(fout, s_merged[i]);
  }
  fout.close();

  if (LittleFS.exists(kPath) && !LittleFS.remove(kPath)) {
    Serial.println("[route_cache] remove failed (file busy?)");
    LittleFS.remove(kTmpPath);
    noteFlushAttempt(now_ms);
    return;
  }
  if (!LittleFS.rename(kTmpPath, kPath)) {
    Serial.println("[route_cache] rename failed");
    noteFlushAttempt(now_ms);
    return;
  }

  s_dirty = false;
  noteFlushAttempt(now_ms);
  Serial.printf("[route_cache] saved %u entries\n", static_cast<unsigned>(merged_count));
}

void sendDownload(WebServer* server) {
  if (server == nullptr) {
    return;
  }
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(5000))) {
    server->send(503, "text/plain; charset=utf-8", "Route cache busy, try again");
    return;
  }
  if (!ensureMounted() || !LittleFS.exists(kPath)) {
    server->send(404, "text/plain; charset=utf-8", "Route cache file not found");
    return;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    server->send(500, "text/plain; charset=utf-8", "Could not open route cache");
    return;
  }

  server->sendHeader("Content-Disposition", "attachment; filename=\"route_cache.csv\"");
  server->sendHeader("Cache-Control", "no-store");
  server->streamFile(f, "text/csv; charset=utf-8");
  f.close();
}

bool flashUsage(size_t* used_bytes, size_t* total_bytes) {
  if (used_bytes != nullptr) {
    *used_bytes = 0;
  }
  if (total_bytes != nullptr) {
    *total_bytes = 0;
  }
  FsLock lock;
  if (!lock.acquire(0)) {
    return false;
  }
  if (!ensureMounted()) {
    return false;
  }
  size_t used = 0;
  size_t total = 0;
  used = LittleFS.usedBytes();
  total = LittleFS.totalBytes();
  if (total == 0) {
    return false;
  }
  if (used_bytes != nullptr) {
    *used_bytes = used;
  }
  if (total_bytes != nullptr) {
    *total_bytes = total;
  }
  return true;
}

}  // namespace services::route_cache
