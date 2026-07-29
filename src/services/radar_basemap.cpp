#include "services/radar_basemap.h"

#include <Arduino.h>
#include <FS.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>

#include "services/map_center.h"
#include "services/route_cache_store.h"
#include "ui/radar_scale.h"

namespace services::basemap {

namespace {

constexpr char kPath[] = "/basemap.jpg";
constexpr char kTmpPath[] = "/basemap.tmp";
constexpr char kStoreNs[] = "fs_basemap";
constexpr char kEnKey[] = "en";
constexpr char kLatKey[] = "lat";
constexpr char kLonKey[] = "lon";
constexpr char kMiKey[] = "mi";
constexpr char kFacingKey[] = "fac";
constexpr char kValidKey[] = "ok";

bool s_enabled = false;
Meta s_meta{};

uint16_t* s_cache = nullptr;
bool s_cache_valid = false;

File s_upload;
bool s_upload_active = false;
size_t s_upload_bytes = 0;
bool s_upload_failed = false;

JPEGDEC s_jpeg;

struct DrawCtx {
  uint16_t* pixels = nullptr;
  int w = 0;
  int h = 0;
};

DrawCtx s_draw_ctx;

void* psramAlloc(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void psramFree(void* p) {
  if (p != nullptr) {
    heap_caps_free(p);
  }
}

bool ensureFs() { return services::route_cache::mount(); }

void persistEnabled() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kEnKey, s_enabled);
    prefs.end();
  }
}

void persistMeta() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  prefs.putBool(kValidKey, s_meta.valid);
  if (s_meta.valid) {
    prefs.putDouble(kLatKey, s_meta.lat);
    prefs.putDouble(kLonKey, s_meta.lon);
    prefs.putUChar(kMiKey, s_meta.range_miles);
    prefs.putUShort(kFacingKey, s_meta.facing_deg);
  }
  prefs.end();
}

void loadPrefs() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  s_enabled = prefs.getBool(kEnKey, false);
  s_meta.valid = prefs.getBool(kValidKey, false);
  if (s_meta.valid) {
    s_meta.lat = prefs.getDouble(kLatKey, 0);
    s_meta.lon = prefs.getDouble(kLonKey, 0);
    s_meta.range_miles = prefs.getUChar(kMiKey, 0);
    s_meta.facing_deg = prefs.getUShort(kFacingKey, 0);
  }
  prefs.end();
}

void freeCache() {
  psramFree(s_cache);
  s_cache = nullptr;
  s_cache_valid = false;
}

int jpegDrawCallback(JPEGDRAW* draw) {
  DrawCtx* ctx = &s_draw_ctx;
  if (ctx->pixels == nullptr || draw == nullptr || draw->pPixels == nullptr) {
    return 0;
  }
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

bool decodeFileToCache() {
  freeCache();
  if (!ensureFs() || !LittleFS.exists(kPath)) {
    return false;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    return false;
  }
  const size_t len = f.size();
  if (len < 64 || len > kMaxJpegBytes) {
    f.close();
    Serial.printf("[basemap] reject size %u\n", static_cast<unsigned>(len));
    return false;
  }

  uint8_t* jpeg = static_cast<uint8_t*>(psramAlloc(len));
  if (jpeg == nullptr) {
    f.close();
    Serial.println("[basemap] jpeg alloc failed");
    return false;
  }
  const size_t got = f.read(jpeg, len);
  f.close();
  if (got != len) {
    psramFree(jpeg);
    return false;
  }

  const size_t px_bytes =
      static_cast<size_t>(kPixelSize) * static_cast<size_t>(kPixelSize) * sizeof(uint16_t);
  s_cache = static_cast<uint16_t*>(psramAlloc(px_bytes));
  if (s_cache == nullptr) {
    psramFree(jpeg);
    Serial.println("[basemap] pixel alloc failed");
    return false;
  }
  memset(s_cache, 0, px_bytes);

  s_draw_ctx.pixels = s_cache;
  s_draw_ctx.w = kPixelSize;
  s_draw_ctx.h = kPixelSize;

  if (!s_jpeg.openRAM(jpeg, static_cast<int>(len), jpegDrawCallback)) {
    Serial.println("[basemap] jpeg open failed");
    psramFree(jpeg);
    freeCache();
    return false;
  }
  s_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  const int jw = s_jpeg.getWidth();
  const int jh = s_jpeg.getHeight();
  if (jw < kPixelSize - 8 || jh < kPixelSize - 8 || jw > kPixelSize + 16 ||
      jh > kPixelSize + 16) {
    Serial.printf("[basemap] unexpected size %dx%d (want %d)\n", jw, jh, kPixelSize);
    s_jpeg.close();
    psramFree(jpeg);
    freeCache();
    return false;
  }
  const int ok = s_jpeg.decode(0, 0, 0);
  s_jpeg.close();
  psramFree(jpeg);
  s_draw_ctx.pixels = nullptr;
  if (ok != 1) {
    Serial.println("[basemap] jpeg decode failed");
    freeCache();
    return false;
  }
  s_cache_valid = true;
  Serial.printf("[basemap] decoded %dx%d (%u jpeg bytes)\n", kPixelSize, kPixelSize,
                static_cast<unsigned>(len));
  return true;
}

bool anglesNear(uint16_t a, uint16_t b) {
  int d = static_cast<int>(a) - static_cast<int>(b);
  while (d < -180) {
    d += 360;
  }
  while (d > 180) {
    d -= 360;
  }
  return d >= -2 && d <= 2;
}

}  // namespace

void init() {
  ensureFs();
  loadPrefs();
  if (s_enabled && hasImage() && metaMatchesLive()) {
    // Lazy decode on first blit — avoid boot cost.
  }
}

bool enabled() { return s_enabled; }

void saveEnabledFromForm(const char* checkbox_value) {
  bool on = false;
  if (checkbox_value != nullptr && checkbox_value[0] != '\0') {
    if ((checkbox_value[0] == 'T' || checkbox_value[0] == 't') && checkbox_value[1] == '\0') {
      on = true;
    } else if (strcmp(checkbox_value, "on") == 0) {
      on = true;
    }
  }
  s_enabled = on;
  persistEnabled();
}

void invalidateCache() { freeCache(); }

Meta storedMeta() { return s_meta; }

bool hasImage() {
  if (!ensureFs()) {
    return false;
  }
  return LittleFS.exists(kPath) && s_meta.valid;
}

bool metaMatchesLive() {
  if (!s_meta.valid) {
    return false;
  }
  const double dlat = fabs(s_meta.lat - services::map_center::latitude());
  const double dlon = fabs(s_meta.lon - services::map_center::longitude());
  if (dlat > 0.00015 || dlon > 0.00015) {
    return false;
  }
  if (s_meta.range_miles != ui::radar::scaleActiveMiles()) {
    return false;
  }
  if (!anglesNear(s_meta.facing_deg, ui::radar::facingDeg())) {
    return false;
  }
  return true;
}

bool blitRgb565(uint16_t* dst, int w, int h) {
  if (dst == nullptr || w != kPixelSize || h != kPixelSize) {
    return false;
  }
  if (!s_enabled || !hasImage() || !metaMatchesLive()) {
    return false;
  }
  if (!s_cache_valid && !decodeFileToCache()) {
    return false;
  }
  memcpy(dst, s_cache,
         static_cast<size_t>(kPixelSize) * static_cast<size_t>(kPixelSize) * sizeof(uint16_t));
  return true;
}

void statusText(char* buf, size_t len) {
  if (buf == nullptr || len == 0) {
    return;
  }
  if (!hasImage()) {
    snprintf(buf, len, "No basemap on flash.");
    return;
  }
  if (!metaMatchesLive()) {
    snprintf(buf, len,
             "Stored: %.4f,%.4f @ %u mi facing %u — does not match current radar "
             "(regenerate after changing center/range/facing).",
             s_meta.lat, s_meta.lon, static_cast<unsigned>(s_meta.range_miles),
             static_cast<unsigned>(s_meta.facing_deg));
    return;
  }
  snprintf(buf, len, "Ready: %.4f,%.4f @ %u mi facing %u%s", s_meta.lat, s_meta.lon,
           static_cast<unsigned>(s_meta.range_miles),
           static_cast<unsigned>(s_meta.facing_deg), s_enabled ? " (enabled)" : " (disabled)");
}

void uploadBegin() {
  uploadAbort();
  if (!ensureFs()) {
    s_upload_failed = true;
    return;
  }
  LittleFS.remove(kTmpPath);
  s_upload = LittleFS.open(kTmpPath, "w");
  if (!s_upload) {
    s_upload_failed = true;
    return;
  }
  s_upload_active = true;
  s_upload_bytes = 0;
  s_upload_failed = false;
}

bool uploadWrite(const uint8_t* data, size_t len) {
  if (!s_upload_active || s_upload_failed || !s_upload) {
    return false;
  }
  if (s_upload_bytes + len > kMaxJpegBytes) {
    Serial.println("[basemap] upload too large");
    s_upload_failed = true;
    uploadAbort();
    return false;
  }
  const size_t wrote = s_upload.write(data, len);
  s_upload_bytes += wrote;
  if (wrote != len) {
    s_upload_failed = true;
    uploadAbort();
    return false;
  }
  return true;
}

bool uploadFinish(size_t total_bytes) {
  if (!s_upload_active || s_upload_failed || !s_upload) {
    uploadAbort();
    return false;
  }
  s_upload.close();
  s_upload_active = false;
  if (total_bytes > 0 && total_bytes != s_upload_bytes) {
    // Browser total can differ; prefer counted bytes.
  }
  if (s_upload_bytes < 64) {
    LittleFS.remove(kTmpPath);
    return false;
  }
  LittleFS.remove(kPath);
  if (!LittleFS.rename(kTmpPath, kPath)) {
    // Some LittleFS builds lack rename — copy fallback.
    File src = LittleFS.open(kTmpPath, "r");
    File dst = LittleFS.open(kPath, "w");
    if (!src || !dst) {
      if (src) {
        src.close();
      }
      if (dst) {
        dst.close();
      }
      LittleFS.remove(kTmpPath);
      return false;
    }
    uint8_t buf[512];
    while (src.available()) {
      const int n = src.read(buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      dst.write(buf, static_cast<size_t>(n));
    }
    src.close();
    dst.close();
    LittleFS.remove(kTmpPath);
  }

  s_meta.lat = services::map_center::latitude();
  s_meta.lon = services::map_center::longitude();
  s_meta.range_miles = ui::radar::scaleActiveMiles();
  s_meta.facing_deg = ui::radar::facingDeg();
  s_meta.valid = true;
  persistMeta();
  freeCache();
  s_enabled = true;
  persistEnabled();
  Serial.printf("[basemap] saved %u bytes meta=%.5f,%.5f mi=%u fac=%u\n",
                static_cast<unsigned>(s_upload_bytes), s_meta.lat, s_meta.lon,
                static_cast<unsigned>(s_meta.range_miles),
                static_cast<unsigned>(s_meta.facing_deg));
  return true;
}

void uploadAbort() {
  if (s_upload) {
    s_upload.close();
  }
  s_upload_active = false;
  if (ensureFs()) {
    LittleFS.remove(kTmpPath);
  }
}

bool clear() {
  freeCache();
  if (!ensureFs()) {
    return false;
  }
  LittleFS.remove(kPath);
  LittleFS.remove(kTmpPath);
  s_meta = {};
  persistMeta();
  return true;
}

}  // namespace services::basemap
