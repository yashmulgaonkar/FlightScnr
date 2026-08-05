#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace services::basemap {

/** Raster style used when the basemap JPEG was baked. */
enum class Style : uint8_t {
  Dark = 0,     // CARTO Dark Matter, no place labels
  Light = 1,    // CARTO Positron, no place labels
  Vfr = 2,      // FAA VFR Sectional
  Voyager = 3,  // CARTO Voyager, no place labels (richer light map)
};

/** Call after LittleFS is mountable (route_cache::mount). Loads NVS prefs. */
void init();

/** True when the user enabled basemap and a matching JPEG is on flash. */
bool enabled();

/** Persist enable checkbox from settings form ("T"/missing). */
void saveEnabledFromForm(const char* checkbox_value);

/**
 * Bake adjustments (browser-side when generating; stored in NVS for the portal).
 * Dark/light: contrast 0–200% (100 = unchanged, lower flattens, higher boosts).
 * VFR: wash 0–100% toward white.
 */
uint8_t contrastPercentDark();
uint8_t contrastPercentLight();
uint8_t washPercentVfr();
void saveBakeAdjustFromForm(const char* dark_contrast_pct, const char* light_contrast_pct,
                            const char* vfr_wash_pct);

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
  Style style = Style::Dark;
  bool valid = false;
};

Meta storedMeta();

/** Style stamped on the stored bake (Dark if none). */
Style storedStyle();

/** True when enabled + valid bake should appear under the radar. */
bool wantsDisplay();

/** True when the decoded RGB565 cache is ready in PSRAM. */
bool cacheReady();

/**
 * Stored center/facing match live, and live range ≤ baked coverage miles.
 * Zooming in does not require regenerate; zooming past bake or moving center does.
 */
bool metaMatchesLive();

/**
 * Begin/abort/finish multipart upload of a baseline JPEG (board pixel size).
 * finish stamps meta from live center/facing, bake coverage miles, and style.
 */
void uploadBegin();
bool uploadWrite(const uint8_t* data, size_t len);
bool uploadFinish(size_t total_bytes, Style style, uint8_t range_miles);
void uploadAbort();

/** Delete /basemap.jpg and clear meta/cache. */
bool clear();

constexpr size_t kMaxJpegBytes = 180 * 1024;
constexpr int kPixelSize = config::kDisplayWidth;

}  // namespace services::basemap
