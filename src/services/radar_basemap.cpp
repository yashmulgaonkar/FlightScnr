#include "services/radar_basemap.h"

#include <Arduino.h>
#include <FS.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>

#include "services/map_center.h"
#include "services/https_lock.h"
#include "services/route_cache_store.h"
#include "ui/radar_display.h"
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
constexpr char kStyleKey[] = "sty";
constexpr char kValidKey[] = "ok";
constexpr char kContrastDarkKey[] = "ctr_d";
constexpr char kContrastLightKey[] = "ctr_l";
constexpr char kWashVfrKey[] = "wsh_v";

constexpr uint8_t kDefaultContrastDarkPct = 100;
constexpr uint8_t kDefaultContrastLightPct = 100;
constexpr uint8_t kDefaultWashVfrPct = 55;
constexpr uint8_t kMaxContrastPct = 200;

/** Skip JPEG decode on the loop task when memory/TLS is tight (decode can stall
 *  the UI for seconds and fights ADS-B for internal heap). */
constexpr uint32_t kMinFreeHeapToDecode = 22000;
constexpr uint32_t kMinContigHeapToDecode = 10000;

bool s_enabled = false;
Meta s_meta{};
uint8_t s_contrast_dark_pct = kDefaultContrastDarkPct;
uint8_t s_contrast_light_pct = kDefaultContrastLightPct;
uint8_t s_wash_vfr_pct = kDefaultWashVfrPct;

uint16_t* s_cache = nullptr;
bool s_cache_valid = false;

/** Cache LittleFS presence — exists()/open on a missing path spams VFS errors
 *  and was hit from every stalled radar full-draw retry. */
enum class FileState : uint8_t { Unknown, Present, Absent };
FileState s_file_state = FileState::Unknown;

File s_upload;
bool s_upload_active = false;
size_t s_upload_bytes = 0;
bool s_upload_failed = false;

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
    prefs.putUChar(kStyleKey, static_cast<uint8_t>(s_meta.style));
  }
  prefs.end();
}

void loadPrefs() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  s_enabled = prefs.getBool(kEnKey, false);
  s_contrast_dark_pct = prefs.getUChar(kContrastDarkKey, kDefaultContrastDarkPct);
  s_contrast_light_pct = prefs.getUChar(kContrastLightKey, kDefaultContrastLightPct);
  s_wash_vfr_pct = prefs.getUChar(kWashVfrKey, kDefaultWashVfrPct);
  if (s_contrast_dark_pct > kMaxContrastPct) {
    s_contrast_dark_pct = kMaxContrastPct;
  }
  if (s_contrast_light_pct > kMaxContrastPct) {
    s_contrast_light_pct = kMaxContrastPct;
  }
  if (s_wash_vfr_pct > 100) {
    s_wash_vfr_pct = 100;
  }
  s_meta.valid = prefs.getBool(kValidKey, false);
  if (s_meta.valid) {
    s_meta.lat = prefs.getDouble(kLatKey, 0);
    s_meta.lon = prefs.getDouble(kLonKey, 0);
    s_meta.range_miles = prefs.getUChar(kMiKey, 0);
    s_meta.facing_deg = prefs.getUShort(kFacingKey, 0);
    const uint8_t sty = prefs.getUChar(kStyleKey, static_cast<uint8_t>(Style::Dark));
    if (sty == static_cast<uint8_t>(Style::Light)) {
      s_meta.style = Style::Light;
    } else if (sty == static_cast<uint8_t>(Style::Vfr)) {
      s_meta.style = Style::Vfr;
    } else if (sty == static_cast<uint8_t>(Style::Voyager)) {
      s_meta.style = Style::Voyager;
    } else {
      s_meta.style = Style::Dark;
    }
  }
  prefs.end();
}

void freeCache() {
  psramFree(s_cache);
  s_cache = nullptr;
  s_cache_valid = false;
}

void noteFilePresent() { s_file_state = FileState::Present; }
void noteFileAbsent() { s_file_state = FileState::Absent; }
void noteFileUnknown() { s_file_state = FileState::Unknown; }

bool filePresent() {
  if (s_file_state == FileState::Present) {
    return true;
  }
  if (s_file_state == FileState::Absent) {
    return false;
  }
  if (!ensureFs()) {
    noteFileAbsent();
    return false;
  }
  // Single probe — ESP32 LittleFS.exists() opens the path and logs on miss.
  if (LittleFS.exists(kPath)) {
    noteFilePresent();
    return true;
  }
  noteFileAbsent();
  return false;
}

void clearMetaIfOrphaned() {
  if (!s_meta.valid) {
    return;
  }
  if (filePresent()) {
    return;
  }
  // NVS says we have a bake but the JPEG is gone — stop retrying.
  s_meta = {};
  persistMeta();
  if (s_enabled) {
    s_enabled = false;
    persistEnabled();
  }
  freeCache();
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
  const unsigned long t0 = millis();
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

  // JPEGDEC embeds large file/huffman buffers — keep it out of .bss / internal RAM.
  void* jpeg_obj_mem = psramAlloc(sizeof(JPEGDEC));
  if (jpeg_obj_mem == nullptr) {
    psramFree(jpeg);
    freeCache();
    Serial.println("[basemap] JPEGDEC alloc failed");
    return false;
  }
  auto* dec = new (jpeg_obj_mem) JPEGDEC();

  if (!dec->openRAM(jpeg, static_cast<int>(len), jpegDrawCallback)) {
    Serial.println("[basemap] jpeg open failed");
    dec->~JPEGDEC();
    psramFree(jpeg_obj_mem);
    psramFree(jpeg);
    freeCache();
    return false;
  }
  dec->setPixelType(RGB565_LITTLE_ENDIAN);
  const int jw = dec->getWidth();
  const int jh = dec->getHeight();
  if (jw < kPixelSize - 8 || jh < kPixelSize - 8 || jw > kPixelSize + 16 ||
      jh > kPixelSize + 16) {
    Serial.printf("[basemap] unexpected size %dx%d (want %d)\n", jw, jh, kPixelSize);
    dec->close();
    dec->~JPEGDEC();
    psramFree(jpeg_obj_mem);
    psramFree(jpeg);
    freeCache();
    return false;
  }
  const unsigned long t_decode = millis();
  const int ok = dec->decode(0, 0, 0);
  const unsigned long decode_ms = millis() - t_decode;
  dec->close();
  dec->~JPEGDEC();
  psramFree(jpeg_obj_mem);
  psramFree(jpeg);
  s_draw_ctx.pixels = nullptr;
  if (ok != 1) {
    Serial.println("[basemap] jpeg decode failed");
    freeCache();
    return false;
  }
  s_cache_valid = true;
  Serial.printf("[basemap] decoded %dx%d jpeg=%uB decode_ms=%lu total_ms=%lu\n", kPixelSize,
                kPixelSize, static_cast<unsigned>(len), decode_ms, millis() - t0);
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
  noteFileUnknown();
  clearMetaIfOrphaned();
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
  const bool changed = on != s_enabled;
  s_enabled = on;
  persistEnabled();
  if (changed) {
    ui::radarDisplayInvalidateBasemap();
  }
}

namespace {

uint8_t parsePct(const char* value, uint8_t fallback, uint8_t max_pct) {
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long v = strtol(value, &end, 10);
  if (end == value) {
    return fallback;
  }
  if (v < 0) {
    return 0;
  }
  if (v > static_cast<long>(max_pct)) {
    return max_pct;
  }
  return static_cast<uint8_t>(v);
}

void persistBakeAdjust() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  prefs.putUChar(kContrastDarkKey, s_contrast_dark_pct);
  prefs.putUChar(kContrastLightKey, s_contrast_light_pct);
  prefs.putUChar(kWashVfrKey, s_wash_vfr_pct);
  prefs.end();
}

}  // namespace

uint8_t contrastPercentDark() { return s_contrast_dark_pct; }
uint8_t contrastPercentLight() { return s_contrast_light_pct; }
uint8_t washPercentVfr() { return s_wash_vfr_pct; }

void saveBakeAdjustFromForm(const char* dark_contrast_pct, const char* light_contrast_pct,
                            const char* vfr_wash_pct) {
  s_contrast_dark_pct = parsePct(dark_contrast_pct, s_contrast_dark_pct, kMaxContrastPct);
  s_contrast_light_pct = parsePct(light_contrast_pct, s_contrast_light_pct, kMaxContrastPct);
  s_wash_vfr_pct = parsePct(vfr_wash_pct, s_wash_vfr_pct, 100);
  persistBakeAdjust();
  Serial.printf("[basemap] contrast dark=%u%% light=%u%% vfr_wash=%u%%\n",
                static_cast<unsigned>(s_contrast_dark_pct),
                static_cast<unsigned>(s_contrast_light_pct),
                static_cast<unsigned>(s_wash_vfr_pct));
}

void invalidateCache() { freeCache(); }

Meta storedMeta() { return s_meta; }

Style storedStyle() { return s_meta.valid ? s_meta.style : Style::Dark; }

bool wantsDisplay() {
  return s_enabled && s_meta.valid && metaMatchesLive() && filePresent();
}

bool cacheReady() { return s_cache_valid; }

bool hasImage() {
  if (!s_meta.valid) {
    return false;
  }
  return filePresent();
}

bool metaMatchesLive() {
  if (!s_meta.valid || s_meta.range_miles == 0) {
    return false;
  }
  const double dlat = fabs(s_meta.lat - services::map_center::latitude());
  const double dlon = fabs(s_meta.lon - services::map_center::longitude());
  if (dlat > 0.00015 || dlon > 0.00015) {
    return false;
  }
  // Bake covers max range; zooming in is OK. Zooming past baked miles is not.
  if (ui::radar::scaleActiveMiles() > s_meta.range_miles) {
    return false;
  }
  if (!anglesNear(s_meta.facing_deg, ui::radar::facingDeg())) {
    return false;
  }
  return true;
}

bool heapOkToDecode() {
  if (services::https::busy()) {
    return false;
  }
  return ESP.getFreeHeap() >= kMinFreeHeapToDecode &&
         ESP.getMaxAllocHeap() >= kMinContigHeapToDecode;
}

bool blitRgb565(uint16_t* dst, int w, int h) {
  const unsigned long t0 = millis();
  if (dst == nullptr || w != kPixelSize || h != kPixelSize) {
    return false;
  }
  if (!s_enabled || !s_meta.valid || !metaMatchesLive()) {
    return false;
  }
  if (!filePresent()) {
    clearMetaIfOrphaned();
    return false;
  }
  if (!s_cache_valid) {
    // Never decode on the radar stall path while TLS is active / heap is low —
    // JPEGDEC blocked the loop for ~18s in the field and left max_blk wedged.
    if (!heapOkToDecode()) {
      Serial.printf("[basemap] decode defer https=%d heap=%u max_blk=%u\n",
                    services::https::busy() ? 1 : 0, ESP.getFreeHeap(),
                    ESP.getMaxAllocHeap());
      return false;
    }
    if (!decodeFileToCache()) {
      return false;
    }
  }

  const uint8_t live_mi = ui::radar::scaleActiveMiles();
  const uint8_t baked_mi = s_meta.range_miles;
  const size_t px =
      static_cast<size_t>(kPixelSize) * static_cast<size_t>(kPixelSize);
  if (live_mi == baked_mi) {
    memcpy(dst, s_cache, px * sizeof(uint16_t));
    Serial.printf("[basemap] blit 1:1 ms=%lu\n", millis() - t0);
    return true;
  }

  // Zoomed in vs bake: bilinear-sample the center crop and stretch to the panel.
  // Nearest-neighbor looked blocky; bilinear softens upscales (no new detail).
  const float scale =
      static_cast<float>(live_mi) / static_cast<float>(baked_mi);
  const float cx = static_cast<float>(kPixelSize - 1) * 0.5f;
  const float cy = static_cast<float>(kPixelSize - 1) * 0.5f;
  const int lim = kPixelSize - 1;

  auto sample565 = [&](int sx, int sy) -> uint16_t {
    if (sx < 0) {
      sx = 0;
    } else if (sx > lim) {
      sx = lim;
    }
    if (sy < 0) {
      sy = 0;
    } else if (sy > lim) {
      sy = lim;
    }
    return s_cache[static_cast<size_t>(sy) * static_cast<size_t>(kPixelSize) +
                   static_cast<size_t>(sx)];
  };
  auto unpack = [](uint16_t c, int* r, int* g, int* b) {
    *r = (c >> 11) & 0x1F;
    *g = (c >> 5) & 0x3F;
    *b = c & 0x1F;
  };
  auto pack = [](int r, int g, int b) -> uint16_t {
    if (r < 0) {
      r = 0;
    } else if (r > 31) {
      r = 31;
    }
    if (g < 0) {
      g = 0;
    } else if (g > 63) {
      g = 63;
    }
    if (b < 0) {
      b = 0;
    } else if (b > 31) {
      b = 31;
    }
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
  };

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float sx_f = cx + (static_cast<float>(x) - cx) * scale;
      const float sy_f = cy + (static_cast<float>(y) - cy) * scale;
      if (sx_f < -1.0f || sy_f < -1.0f || sx_f > static_cast<float>(kPixelSize) ||
          sy_f > static_cast<float>(kPixelSize)) {
        dst[static_cast<size_t>(y) * static_cast<size_t>(w) +
            static_cast<size_t>(x)] = 0;
        continue;
      }
      const int x0 = static_cast<int>(floorf(sx_f));
      const int y0 = static_cast<int>(floorf(sy_f));
      const float tx = sx_f - static_cast<float>(x0);
      const float ty = sy_f - static_cast<float>(y0);
      int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
      unpack(sample565(x0, y0), &r00, &g00, &b00);
      unpack(sample565(x0 + 1, y0), &r10, &g10, &b10);
      unpack(sample565(x0, y0 + 1), &r01, &g01, &b01);
      unpack(sample565(x0 + 1, y0 + 1), &r11, &g11, &b11);
      const float r0 = static_cast<float>(r00) + (static_cast<float>(r10 - r00) * tx);
      const float r1 = static_cast<float>(r01) + (static_cast<float>(r11 - r01) * tx);
      const float g0 = static_cast<float>(g00) + (static_cast<float>(g10 - g00) * tx);
      const float g1 = static_cast<float>(g01) + (static_cast<float>(g11 - g01) * tx);
      const float b0 = static_cast<float>(b00) + (static_cast<float>(b10 - b00) * tx);
      const float b1 = static_cast<float>(b01) + (static_cast<float>(b11 - b01) * tx);
      const int r = static_cast<int>(lroundf(r0 + (r1 - r0) * ty));
      const int g = static_cast<int>(lroundf(g0 + (g1 - g0) * ty));
      const int b = static_cast<int>(lroundf(b0 + (b1 - b0) * ty));
      dst[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
          pack(r, g, b);
    }
  }
  Serial.printf("[basemap] blit zoom live=%u bake=%u bilinear ms=%lu\n",
                static_cast<unsigned>(live_mi), static_cast<unsigned>(baked_mi),
                millis() - t0);
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
  const char* style_name = "dark";
  if (s_meta.style == Style::Light) {
    style_name = "light";
  } else if (s_meta.style == Style::Vfr) {
    style_name = "vfr";
  } else if (s_meta.style == Style::Voyager) {
    style_name = "voyager";
  }
  if (!metaMatchesLive()) {
    const uint8_t live_mi = ui::radar::scaleActiveMiles();
    if (live_mi > s_meta.range_miles) {
      snprintf(buf, len,
               "Stored %s @ %u mi — live range %u mi exceeds bake (regenerate).",
               style_name, static_cast<unsigned>(s_meta.range_miles),
               static_cast<unsigned>(live_mi));
    } else {
      snprintf(buf, len,
               "Stored %s: %.4f,%.4f @ %u mi facing %u — does not match current "
               "center/facing (regenerate).",
               style_name, s_meta.lat, s_meta.lon,
               static_cast<unsigned>(s_meta.range_miles),
               static_cast<unsigned>(s_meta.facing_deg));
    }
    return;
  }
  const uint8_t live_mi = ui::radar::scaleActiveMiles();
  if (live_mi < s_meta.range_miles) {
    snprintf(buf, len,
             "Ready (%s): %.4f,%.4f bake %u mi (showing %u mi zoom) facing %u%s",
             style_name, s_meta.lat, s_meta.lon,
             static_cast<unsigned>(s_meta.range_miles), static_cast<unsigned>(live_mi),
             static_cast<unsigned>(s_meta.facing_deg),
             s_enabled ? " (enabled)" : " (disabled)");
  } else {
    snprintf(buf, len, "Ready (%s): %.4f,%.4f @ %u mi facing %u%s", style_name,
             s_meta.lat, s_meta.lon, static_cast<unsigned>(s_meta.range_miles),
             static_cast<unsigned>(s_meta.facing_deg),
             s_enabled ? " (enabled)" : " (disabled)");
  }
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

bool uploadFinish(size_t total_bytes, Style style, uint8_t range_miles) {
  if (!s_upload_active || s_upload_failed || !s_upload) {
    uploadAbort();
    return false;
  }
  s_upload.close();
  s_upload_active = false;
  (void)total_bytes;
  if (s_upload_bytes < 64) {
    LittleFS.remove(kTmpPath);
    return false;
  }
  LittleFS.remove(kPath);
  if (!LittleFS.rename(kTmpPath, kPath)) {
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

  noteFilePresent();
  s_meta.lat = services::map_center::latitude();
  s_meta.lon = services::map_center::longitude();
  uint8_t mi = range_miles;
  bool mi_ok = false;
  for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
    if (ui::radar::kRangeMileOptions[i] == mi) {
      mi_ok = true;
      break;
    }
  }
  if (!mi_ok) {
    mi = ui::radar::scaleActiveMiles();
    for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
      if (ui::radar::kRangeMileOptions[i] == mi) {
        mi_ok = true;
        break;
      }
    }
    if (!mi_ok) {
      mi = ui::radar::kRangeMileOptions[ui::radar::kRangeMileOptionCount - 1];
    }
  }
  s_meta.range_miles = mi;
  s_meta.facing_deg = ui::radar::facingDeg();
  if (style == Style::Light) {
    s_meta.style = Style::Light;
  } else if (style == Style::Vfr) {
    s_meta.style = Style::Vfr;
  } else if (style == Style::Voyager) {
    s_meta.style = Style::Voyager;
  } else {
    s_meta.style = Style::Dark;
  }
  s_meta.valid = true;
  persistMeta();
  freeCache();
  s_enabled = true;
  persistEnabled();
  Serial.printf("[basemap] saved %u bytes meta=%.5f,%.5f mi=%u fac=%u sty=%u\n",
                static_cast<unsigned>(s_upload_bytes), s_meta.lat, s_meta.lon,
                static_cast<unsigned>(s_meta.range_miles),
                static_cast<unsigned>(s_meta.facing_deg),
                static_cast<unsigned>(s_meta.style));
  ui::radarDisplayInvalidateBasemap();
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
    noteFileAbsent();
    return false;
  }
  LittleFS.remove(kPath);
  LittleFS.remove(kTmpPath);
  noteFileAbsent();
  s_meta = {};
  persistMeta();
  s_enabled = false;
  persistEnabled();
  ui::radarDisplayInvalidateBasemap();
  return true;
}

}  // namespace services::basemap
