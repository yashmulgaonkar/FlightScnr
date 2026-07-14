#include "services/aircraft_photo.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>
#include <JPEGDEC.h>

#include <esp_heap_caps.h>

#include <cctype>
#include <cstring>

#include "config.h"
#include "hardware/plane_gfx.h"
#include "services/adsb_client.h"
#include "services/https_heap.h"
#include "services/https_lock.h"

namespace services::photo {

namespace {

constexpr size_t kMaxThumbUrl = 196;
constexpr size_t kMaxPhotographer = 40;
constexpr size_t kMaxJpegBytes = 96 * 1024;
constexpr size_t kMaxProxyUrl = 280;
constexpr int kMaxPhotoW = 220;
constexpr int kMaxPhotoH = 180;
constexpr size_t kMetaCacheSize = 24;
constexpr unsigned long kMetaCacheTtlMs = 24UL * 60UL * 60UL * 1000UL;
constexpr unsigned long kMetaNegativeTtlMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kFetchRetryCooldownMs = 15000UL;
/** Heap/TLS busy — retry soon; do not apply the hard 15s cooldown. */
constexpr unsigned long kFetchSoftRetryMs = 600UL;
/** Heap still tight after a drain — back off longer to avoid log/CPU spam. */
constexpr unsigned long kFetchHeapRetryMs = 1800UL;

struct MetaEntry {
  char hex[7] = {};
  bool resolved = false;
  bool has_photo = false;
  char photographer[kMaxPhotographer] = {};
  char thumb_url[kMaxThumbUrl] = {};
  unsigned long resolved_ms = 0;
};

struct DecodedPhoto {
  char callsign[9] = {};
  char hex[7] = {};
  char photographer[kMaxPhotographer] = {};
  uint16_t* pixels = nullptr;
  int w = 0;
  int h = 0;
  bool valid = false;
};

char s_wanted_callsign[9] = {};
char s_wanted_hex[7] = {};
uint32_t s_generation = 0;

char s_debounce_callsign[9] = {};
char s_debounce_hex[7] = {};
unsigned long s_debounce_due_ms = 0;
bool s_debounce_pending = false;

bool s_job_queued = false;
bool s_job_running = false;
uint32_t s_job_generation = 0;
char s_job_callsign[9] = {};
char s_job_hex[7] = {};

bool s_ready = false;
bool s_ready_ok = false;
char s_ready_callsign[9] = {};
unsigned long s_retry_after_ms = 0;

MetaEntry s_meta[kMetaCacheSize];
size_t s_meta_next = 0;

DecodedPhoto s_decoded;
JPEGDEC s_jpeg;

struct JpegDrawCtx {
  uint16_t* pixels;
  int w;
  int h;
};

/** Large photo/JPEG buffers must stay in PSRAM — never carve internal heap. */
void* psramAllocOnly(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void psramFree(void* p) {
  if (p != nullptr) {
    heap_caps_free(p);
  }
}

void clearDecoded() {
  if (s_decoded.pixels != nullptr) {
    psramFree(s_decoded.pixels);
    s_decoded.pixels = nullptr;
  }
  s_decoded = {};
  // Do not touch s_jpeg_ctx — an in-flight decode owns that buffer and frees it.
}

bool selectionMatches(const char* callsign, uint32_t generation) {
  return generation == s_generation && callsign != nullptr && callsign[0] != '\0' &&
         strcmp(callsign, s_wanted_callsign) == 0;
}

void normalizeHex(char* hex, size_t len) {
  if (hex == nullptr || len == 0) {
    return;
  }
  for (size_t i = 0; hex[i] != '\0' && i + 1 < len; ++i) {
    hex[i] = static_cast<char>(toupper(static_cast<unsigned char>(hex[i])));
  }
}

MetaEntry* findMeta(const char* hex) {
  if (hex == nullptr || hex[0] == '\0') {
    return nullptr;
  }
  const unsigned long now = millis();
  for (size_t i = 0; i < kMetaCacheSize; ++i) {
    if (!s_meta[i].resolved || strcmp(s_meta[i].hex, hex) != 0) {
      continue;
    }
    const unsigned long ttl =
        s_meta[i].has_photo ? kMetaCacheTtlMs : kMetaNegativeTtlMs;
    if (now - s_meta[i].resolved_ms > ttl) {
      s_meta[i] = {};
      continue;
    }
    return &s_meta[i];
  }
  return nullptr;
}

MetaEntry* allocMeta(const char* hex) {
  MetaEntry* existing = findMeta(hex);
  if (existing != nullptr) {
    return existing;
  }
  MetaEntry& slot = s_meta[s_meta_next];
  s_meta_next = (s_meta_next + 1) % kMetaCacheSize;
  slot = {};
  strncpy(slot.hex, hex, sizeof(slot.hex) - 1);
  return &slot;
}

void cacheNegativeMeta(const char* hex) {
  if (hex == nullptr || hex[0] == '\0') {
    return;
  }
  MetaEntry* slot = allocMeta(hex);
  if (slot == nullptr) {
    return;
  }
  slot->resolved = true;
  slot->has_photo = false;
  slot->photographer[0] = '\0';
  slot->thumb_url[0] = '\0';
  slot->resolved_ms = millis();
  strncpy(slot->hex, hex, sizeof(slot->hex) - 1);
}

struct PsramPayload {
  uint8_t* data = nullptr;
  size_t len = 0;
  size_t cap = 0;

  ~PsramPayload() { clear(); }

  void clear() {
    if (data != nullptr) {
      psramFree(data);
      data = nullptr;
    }
    len = 0;
    cap = 0;
  }

  bool reserve(size_t want) {
    if (want <= cap) {
      return true;
    }
    size_t next = cap == 0 ? 4096 : cap;
    while (next < want) {
      next *= 2;
    }
    if (next > kMaxJpegBytes + 4096) {
      next = want;
    }
    // JPEG payloads are large — PSRAM only so TLS/UI keep internal DRAM.
    void* p = heap_caps_realloc(data, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
      return false;
    }
    data = static_cast<uint8_t*>(p);
    cap = next;
    return true;
  }

  bool append(const uint8_t* src, size_t n) {
    if (n == 0) {
      return true;
    }
    if (len + n > kMaxJpegBytes) {
      return false;
    }
    if (!reserve(len + n)) {
      return false;
    }
    memcpy(data + len, src, n);
    len += n;
    return true;
  }
};

bool readHttpPayload(HTTPClient& http, PsramPayload* payload, uint32_t timeout_ms) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr || payload == nullptr) {
    return false;
  }
  const int content_len = http.getSize();
  if (content_len > 0) {
    if (static_cast<size_t>(content_len) > kMaxJpegBytes) {
      return false;
    }
    if (!payload->reserve(static_cast<size_t>(content_len))) {
      return false;
    }
  }
  const unsigned long start = millis();
  uint8_t buf[1024];
  while (http.connected() || stream->available()) {
    if (!selectionMatches(s_job_callsign, s_job_generation)) {
      return false;
    }
    if (millis() - start > timeout_ms) {
      return false;
    }
    const size_t avail = stream->available();
    if (avail == 0) {
      delay(1);
      continue;
    }
    const size_t n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n == 0) {
      delay(1);
      continue;
    }
    if (!payload->append(buf, n)) {
      return false;
    }
    if (content_len > 0 && payload->len >= static_cast<size_t>(content_len)) {
      break;
    }
  }
  return payload->len > 0;
}

JpegDrawCtx s_jpeg_ctx = {};

int jpegDrawCallback(JPEGDRAW* draw) {
  JpegDrawCtx* ctx = &s_jpeg_ctx;
  if (ctx->pixels == nullptr || draw == nullptr || draw->pPixels == nullptr) {
    return 0;
  }

  // iWidth is the source stride (MCU-aligned). iWidthUsed is the visible pixel
  // count for this block. Never skip the whole MCU when stride > remaining
  // width — that left the framebuffer empty except a speck of noise.
  const int src_stride = draw->iWidth;
  int copy_w = (draw->iWidthUsed > 0) ? draw->iWidthUsed : draw->iWidth;
  int src_x = 0;
  int dst_x = draw->x;
  if (dst_x < 0) {
    src_x = -dst_x;
    copy_w += dst_x;
    dst_x = 0;
  }
  if (dst_x + copy_w > ctx->w) {
    copy_w = ctx->w - dst_x;
  }
  if (copy_w <= 0 || src_x >= src_stride) {
    return 1;
  }

  for (int row = 0; row < draw->iHeight; ++row) {
    const int dy = draw->y + row;
    if (dy < 0 || dy >= ctx->h) {
      continue;
    }
    const uint16_t* src = draw->pPixels + row * src_stride + src_x;
    uint16_t* dst = ctx->pixels + static_cast<size_t>(dy) * static_cast<size_t>(ctx->w) +
                    static_cast<size_t>(dst_x);
    memcpy(dst, src, static_cast<size_t>(copy_w) * sizeof(uint16_t));
  }
  return 1;
}

bool decodeJpegToFrame(uint8_t* jpeg, size_t jpeg_len, const char* callsign,
                       const char* hex, const char* photographer) {
  if (jpeg == nullptr || jpeg_len == 0) {
    return false;
  }
  clearDecoded();

  if (!s_jpeg.openRAM(jpeg, static_cast<int>(jpeg_len), jpegDrawCallback)) {
    Serial.println("[photo] jpeg open failed");
    return false;
  }
  s_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

  const int w = s_jpeg.getWidth();
  const int h = s_jpeg.getHeight();
  if (w <= 0 || h <= 0 || w > kMaxPhotoW || h > kMaxPhotoH) {
    Serial.printf("[photo] jpeg size rejected %dx%d\n", w, h);
    s_jpeg.close();
    return false;
  }
  Serial.printf("[photo] jpeg decode %dx%d\n", w, h);

  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  s_jpeg_ctx.pixels = static_cast<uint16_t*>(psramAllocOnly(pixels * sizeof(uint16_t)));
  if (s_jpeg_ctx.pixels == nullptr) {
    Serial.println("[photo] frame alloc failed (PSRAM)");
    s_jpeg.close();
    return false;
  }
  s_jpeg_ctx.w = w;
  s_jpeg_ctx.h = h;
  memset(s_jpeg_ctx.pixels, 0, pixels * sizeof(uint16_t));

  const int ok = s_jpeg.decode(0, 0, 0);
  s_jpeg.close();
  uint16_t* frame = s_jpeg_ctx.pixels;
  s_jpeg_ctx = {};
  if (!ok) {
    psramFree(frame);
    return false;
  }
  if (!selectionMatches(callsign, s_job_generation)) {
    psramFree(frame);
    return false;
  }

  strncpy(s_decoded.callsign, callsign, sizeof(s_decoded.callsign) - 1);
  strncpy(s_decoded.hex, hex, sizeof(s_decoded.hex) - 1);
  if (photographer != nullptr) {
    strncpy(s_decoded.photographer, photographer, sizeof(s_decoded.photographer) - 1);
  }
  s_decoded.pixels = frame;
  s_decoded.w = w;
  s_decoded.h = h;
  s_decoded.valid = true;
  return true;
}

bool fetchJsonMeta(const char* hex, MetaEntry* out) {
  if (hex == nullptr || hex[0] == '\0' || out == nullptr) {
    return false;
  }

  char url[96];
  snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", hex);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(config::kAircraftPhotoTimeoutMs / 1000U);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(config::kAircraftPhotoTimeoutMs);
  http.setUserAgent(config::kPlanespottersUserAgent);
  if (!http.begin(client, url)) {
    return false;
  }

  if (!selectionMatches(s_job_callsign, s_job_generation)) {
    http.end();
    return false;
  }

  const int code = http.GET();
  if (!selectionMatches(s_job_callsign, s_job_generation)) {
    http.end();
    return false;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[photo] meta HTTP %d for %s\n", code, hex);
    http.end();
    return false;
  }

  // Planespotters serves Transfer-Encoding: chunked. Raw stream reads leave hex
  // chunk sizes in the body (ArduinoJson -> InvalidInput). getString() de-chunks.
  const String payload = http.getString();
  http.end();

  if (!selectionMatches(s_job_callsign, s_job_generation)) {
    return false;
  }
  if (payload.isEmpty()) {
    Serial.println("[photo] meta empty body");
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[photo] meta JSON %s len=%u head=%.16s\n", err.c_str(),
                  static_cast<unsigned>(payload.length()), payload.c_str());
    return false;
  }

  out->resolved = true;
  out->resolved_ms = millis();
  out->has_photo = false;
  out->photographer[0] = '\0';
  out->thumb_url[0] = '\0';
  strncpy(out->hex, hex, sizeof(out->hex) - 1);

  JsonArray photos = doc["photos"].as<JsonArray>();
  if (photos.isNull() || photos.size() == 0) {
    return true;
  }
  JsonObject photo = photos[0].as<JsonObject>();
  // Prefer the larger thumb (capsule-radar does the same); weserv resizes on fetch.
  const char* src = photo["thumbnail_large"]["src"] | "";
  if (src[0] == '\0') {
    src = photo["thumbnail"]["src"] | "";
  }
  const char* photographer = photo["photographer"] | "";
  if (src[0] == '\0') {
    return true;
  }
  out->has_photo = true;
  strncpy(out->thumb_url, src, sizeof(out->thumb_url) - 1);
  strncpy(out->photographer, photographer, sizeof(out->photographer) - 1);
  return true;
}

// Planespotters CDN serves progressive JPEGs. JPEGDEC (like TJpgDec) only decodes
// the DC scan for those — a tiny speck. Route through images.weserv.nl to re-encode
// as baseline JPEG sized to our canvas (same approach as capsule-radar).
bool buildBaselineProxyUrl(const char* src_url, char* out, size_t out_len) {
  if (src_url == nullptr || src_url[0] == '\0' || out == nullptr || out_len == 0) {
    return false;
  }
  const char* bare = src_url;
  if (strncmp(bare, "https://", 8) == 0) {
    bare += 8;
  } else if (strncmp(bare, "http://", 7) == 0) {
    bare += 7;
  }
  const int n = snprintf(out, out_len,
                         "https://images.weserv.nl/?url=%s&w=%d&h=%d&fit=inside&output=jpg",
                         bare, kMaxPhotoW, kMaxPhotoH);
  return n > 0 && static_cast<size_t>(n) < out_len;
}

bool fetchJpeg(const char* url, PsramPayload* out) {
  if (url == nullptr || url[0] == '\0' || out == nullptr) {
    return false;
  }

  char proxied[kMaxProxyUrl];
  const char* fetch_url = url;
  if (buildBaselineProxyUrl(url, proxied, sizeof(proxied))) {
    fetch_url = proxied;
  } else {
    Serial.println("[photo] proxy url build failed; trying direct");
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(config::kAircraftPhotoTimeoutMs / 1000U);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(config::kAircraftPhotoTimeoutMs);
  http.setUserAgent(config::kPlanespottersUserAgent);
  if (!http.begin(client, fetch_url)) {
    return false;
  }
  if (!selectionMatches(s_job_callsign, s_job_generation)) {
    http.end();
    return false;
  }

  const int code = http.GET();
  if (!selectionMatches(s_job_callsign, s_job_generation)) {
    http.end();
    return false;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[photo] jpeg HTTP %d\n", code);
    http.end();
    return false;
  }

  const bool ok = readHttpPayload(http, out, config::kAircraftPhotoTimeoutMs);
  http.end();
  return ok && selectionMatches(s_job_callsign, s_job_generation);
}

enum class JobEnd : uint8_t { Ok, SoftFail, HardFail, HeapFail };

// Latched when a job ends HardFail for the current generation (survives consume()).
uint32_t s_hard_done_generation = 0;

void finishJob(JobEnd end) {
  s_job_running = false;
  s_job_queued = false;
  const bool still = selectionMatches(s_job_callsign, s_job_generation);
  // Only back off while this selection is still current. Scrollaway must not
  // punish the next aircraft with a 15s photo cooldown.
  if (end != JobEnd::Ok && still) {
    unsigned long delay_ms = kFetchSoftRetryMs;
    if (end == JobEnd::HardFail) {
      delay_ms = kFetchRetryCooldownMs;
      s_hard_done_generation = s_job_generation;
    } else if (end == JobEnd::HeapFail) {
      delay_ms = kFetchHeapRetryMs;
    }
    s_retry_after_ms = millis() + delay_ms;
  }
  if (!still) {
    clearDecoded();
    s_ready = false;
    return;
  }
  if (end == JobEnd::SoftFail || end == JobEnd::HeapFail) {
    // Stay pending so tickDebounce requeues after the short cooldown.
    s_ready = false;
    return;
  }
  s_ready = true;
  s_ready_ok = end == JobEnd::Ok && s_decoded.valid;
  strncpy(s_ready_callsign, s_job_callsign, sizeof(s_ready_callsign) - 1);
}

void photoJob() {
  s_job_running = true;
  s_job_queued = false;

  if (!selectionMatches(s_job_callsign, s_job_generation) || s_job_hex[0] == '\0') {
    finishJob(JobEnd::SoftFail);
    return;
  }

  if (!services::https::heapReadyForPhoto()) {
    // Route enrich often holds TLS right when detail opens — wait briefly.
    services::https::drainTlsHeapAfterSession(700);
    if (!selectionMatches(s_job_callsign, s_job_generation)) {
      finishJob(JobEnd::SoftFail);
      return;
    }
    if (!services::https::heapReadyForPhoto()) {
      static unsigned long s_last_heap_log_ms = 0;
      const unsigned long now = millis();
      if (static_cast<long>(now - s_last_heap_log_ms) >= 2000) {
        s_last_heap_log_ms = now;
        Serial.printf("[photo] defer: heap low free=%u max_blk=%u\n", ESP.getFreeHeap(),
                      ESP.getMaxAllocHeap());
      }
      finishJob(JobEnd::HeapFail);
      return;
    }
  }

  services::https::ScopedLock tls(config::kAircraftPhotoTimeoutMs + 1000);
  if (!tls.held()) {
    Serial.println("[photo] defer: https busy");
    finishJob(JobEnd::SoftFail);
    return;
  }
  if (!selectionMatches(s_job_callsign, s_job_generation)) {
    finishJob(JobEnd::SoftFail);
    return;
  }

  MetaEntry* meta = findMeta(s_job_hex);
  MetaEntry local = {};
  if (meta == nullptr) {
    if (!fetchJsonMeta(s_job_hex, &local)) {
      // Only cache a miss when this selection is still current (not scrollaway).
      if (selectionMatches(s_job_callsign, s_job_generation)) {
        cacheNegativeMeta(s_job_hex);
      }
      finishJob(JobEnd::HardFail);
      return;
    }
    meta = allocMeta(s_job_hex);
    if (meta != nullptr) {
      *meta = local;
    } else {
      meta = &local;
    }
  }

  if (!selectionMatches(s_job_callsign, s_job_generation)) {
    finishJob(JobEnd::SoftFail);
    return;
  }

  if (!meta->has_photo || meta->thumb_url[0] == '\0') {
    clearDecoded();
    finishJob(JobEnd::HardFail);
    return;
  }

  // Already decoded for this selection (e.g. redraw) — skip download.
  if (s_decoded.valid && strcmp(s_decoded.callsign, s_job_callsign) == 0 &&
      strcmp(s_decoded.hex, s_job_hex) == 0) {
    finishJob(JobEnd::Ok);
    return;
  }

  PsramPayload jpeg;
  if (!fetchJpeg(meta->thumb_url, &jpeg)) {
    // Network/proxy blip or scrollaway mid-download — soft so we retry soon.
    finishJob(JobEnd::SoftFail);
    return;
  }

  const bool decoded =
      decodeJpegToFrame(jpeg.data, jpeg.len, s_job_callsign, s_job_hex, meta->photographer);
  finishJob(decoded ? JobEnd::Ok : JobEnd::HardFail);
  services::https::drainTlsHeapAfterSession(200);
}

bool queuePhotoJob() {
  if (s_wanted_hex[0] == '\0' || s_wanted_callsign[0] == '\0') {
    return false;
  }
  if (s_job_queued || s_job_running) {
    return true;
  }
  if (static_cast<long>(millis() - s_retry_after_ms) < 0) {
    return false;
  }
  // Skip network if we already know there is no photo.
  MetaEntry* known = findMeta(s_wanted_hex);
  if (known != nullptr && known->resolved && !known->has_photo) {
    return false;
  }
  // Skip if already showing this aircraft's photo.
  if (s_decoded.valid && strcmp(s_decoded.callsign, s_wanted_callsign) == 0) {
    s_ready = true;
    s_ready_ok = true;
    strncpy(s_ready_callsign, s_wanted_callsign, sizeof(s_ready_callsign) - 1);
    return true;
  }

  s_job_generation = s_generation;
  strncpy(s_job_callsign, s_wanted_callsign, sizeof(s_job_callsign) - 1);
  strncpy(s_job_hex, s_wanted_hex, sizeof(s_job_hex) - 1);
  s_job_queued = true;
  if (!services::adsb::queueBackgroundJob(&photoJob)) {
    s_job_queued = false;
    return false;
  }
  return true;
}

}  // namespace

void init() {}

void onFlightDetailSelected(const char* callsign, const char* hex, bool immediate) {
  char norm_hex[7] = {};
  if (hex != nullptr) {
    strncpy(norm_hex, hex, sizeof(norm_hex) - 1);
    normalizeHex(norm_hex, sizeof(norm_hex));
  }

  const bool same = callsign != nullptr && callsign[0] != '\0' &&
                    strcmp(callsign, s_wanted_callsign) == 0 &&
                    strcmp(norm_hex, s_wanted_hex) == 0;

  if (!same) {
    ++s_generation;
    s_ready = false;
    s_ready_ok = false;
    s_ready_callsign[0] = '\0';
    s_retry_after_ms = 0;
    s_hard_done_generation = 0;
    // Drop pixels for a different aircraft so scrollaway never shows a stale frame.
    if (!(s_decoded.valid && callsign != nullptr &&
          strcmp(s_decoded.callsign, callsign) == 0)) {
      clearDecoded();
    }
  }

  if (callsign == nullptr || callsign[0] == '\0') {
    s_wanted_callsign[0] = '\0';
    s_wanted_hex[0] = '\0';
    s_debounce_pending = false;
    return;
  }

  strncpy(s_wanted_callsign, callsign, sizeof(s_wanted_callsign) - 1);
  strncpy(s_wanted_hex, norm_hex, sizeof(s_wanted_hex) - 1);

  if (norm_hex[0] == '\0') {
    s_debounce_pending = false;
    return;
  }

  if (immediate) {
    s_debounce_pending = false;
    queuePhotoJob();
    return;
  }

  strncpy(s_debounce_callsign, callsign, sizeof(s_debounce_callsign) - 1);
  strncpy(s_debounce_hex, norm_hex, sizeof(s_debounce_hex) - 1);
  s_debounce_due_ms = millis() + config::kAircraftPhotoDebounceMs;
  s_debounce_pending = true;
}

void tickDebounce(unsigned long now_ms) {
  if (!s_debounce_pending) {
    // Retry queue if a prior attempt failed while ADS-B worker was busy.
    if (!s_job_queued && !s_job_running && s_wanted_hex[0] != '\0' &&
        static_cast<long>(now_ms - s_retry_after_ms) >= 0 &&
        !(s_decoded.valid && strcmp(s_decoded.callsign, s_wanted_callsign) == 0)) {
      MetaEntry* meta = findMeta(s_wanted_hex);
      if (meta == nullptr || (meta->has_photo && meta->thumb_url[0] != '\0')) {
        queuePhotoJob();
      }
    }
    return;
  }
  if (static_cast<long>(now_ms - s_debounce_due_ms) < 0) {
    return;
  }
  s_debounce_pending = false;
  if (strcmp(s_debounce_callsign, s_wanted_callsign) != 0) {
    return;
  }
  queuePhotoJob();
}

void cancel() {
  ++s_generation;
  s_wanted_callsign[0] = '\0';
  s_wanted_hex[0] = '\0';
  s_debounce_pending = false;
  s_job_queued = false;
  s_retry_after_ms = 0;
  s_ready = false;
  s_ready_ok = false;
  s_ready_callsign[0] = '\0';
  s_hard_done_generation = 0;
  clearDecoded();
}

bool ready() { return s_ready; }

bool consume(bool* needs_redraw) {
  if (!s_ready) {
    return false;
  }
  s_ready = false;
  const bool match = selectionMatches(s_ready_callsign, s_generation) ||
                     (s_wanted_callsign[0] != '\0' &&
                      strcmp(s_ready_callsign, s_wanted_callsign) == 0);
  if (needs_redraw != nullptr) {
    *needs_redraw = match && s_ready_ok && s_decoded.valid;
  }
  return match;
}

bool inFlight(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return false;
  }
  if (strcmp(callsign, s_wanted_callsign) != 0) {
    return false;
  }
  return s_debounce_pending || s_job_queued || s_job_running;
}

bool settled(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return true;
  }
  if (strcmp(callsign, s_wanted_callsign) != 0) {
    return false;
  }
  // Empty hex: nothing to fetch.
  if (s_wanted_hex[0] == '\0') {
    return true;
  }
  if (s_debounce_pending || s_job_queued || s_job_running) {
    return false;
  }
  if (s_decoded.valid && strcmp(s_decoded.callsign, callsign) == 0) {
    return true;
  }
  MetaEntry* meta = findMeta(s_wanted_hex);
  if (meta != nullptr && meta->resolved && !meta->has_photo) {
    return true;
  }
  if (s_hard_done_generation == s_generation && s_hard_done_generation != 0) {
    return true;
  }
  // Soft/heap retry cooldown still pending, or never finished a terminal outcome.
  return false;
}

int imageHeight(const char* callsign) {
  if (callsign == nullptr || !s_decoded.valid ||
      strcmp(s_decoded.callsign, callsign) != 0) {
    return 0;
  }
  return s_decoded.h;
}

const char* photographer(const char* callsign) {
  if (callsign == nullptr || !s_decoded.valid ||
      strcmp(s_decoded.callsign, callsign) != 0) {
    return "";
  }
  return s_decoded.photographer;
}

bool draw(PlaneGfx& gfx, const char* callsign, int16_t center_x, int16_t y) {
  if (callsign == nullptr || !s_decoded.valid || s_decoded.pixels == nullptr ||
      strcmp(s_decoded.callsign, callsign) != 0) {
    return false;
  }
  const int16_t x = static_cast<int16_t>(center_x - s_decoded.w / 2);
  gfx.draw16bitRGBBitmap(x, y, s_decoded.pixels, static_cast<int16_t>(s_decoded.w),
                         static_cast<int16_t>(s_decoded.h));
  return true;
}

void releaseBuffers() {
  clearDecoded();
  s_ready = false;
  s_ready_ok = false;
  s_ready_callsign[0] = '\0';
}

}  // namespace services::photo
