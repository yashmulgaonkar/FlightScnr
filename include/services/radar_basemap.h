#pragma once

#include <cstddef>
#include <cstdint>

namespace services::basemap {

/** Call after LittleFS is mountable (route_cache::mount). Loads NVS prefs. */
void init();

/** True when the user enabled basemap and a matching JPEG is on flash. */
bool enabled();

/** Persist enable checkbox from settings form ("T"/missing). */
void saveEnabledFromForm(const char* checkbox_value);

/** Drop cached decode; next blit reloads from LittleFS. */
void invalidateCache();

/**
 * If enabled and metadata matches current map center / facing, and live range
 * is within the baked coverage, copy decoded RGB565 into dst (w*h little-endian).
 * When zoomed in vs bake, crops/scales from center. Returns false → solid fill.
 */
bool blitRgb565(uint16_t* dst, int w, int h);

/** True when /basemap.jpg exists and metadata is present. */
bool hasImage();

/** Human status for the settings portal (into buf). */
void statusText(char* buf, size_t len);

/** Metadata stamped when the image was baked. */
struct Meta {
  double lat = 0;
  double lon = 0;
  uint8_t range_miles = 0;
  uint16_t facing_deg = 0;
  bool valid = false;
};

Meta storedMeta();

/**
 * Stored center/facing match live, and live range ≤ baked coverage miles.
 * Zooming in does not require regenerate; zooming past bake or moving center does.
 */
bool metaMatchesLive();

/**
 * Begin/abort/finish multipart upload of a baseline JPEG (390×390).
 * finish stamps meta from live center/facing and max range (full coverage bake).
 */
void uploadBegin();
bool uploadWrite(const uint8_t* data, size_t len);
bool uploadFinish(size_t total_bytes);
void uploadAbort();

/** Delete /basemap.jpg and clear meta/cache. */
bool clear();

constexpr size_t kMaxJpegBytes = 180 * 1024;
constexpr int kPixelSize = 390;

}  // namespace services::basemap
