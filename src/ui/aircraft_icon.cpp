#include "ui/aircraft_icon.h"

#include <Arduino.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "data/aircraft_icons_lookup.h"
#include "hardware/plane_gfx.h"
#include "services/adsb_client.h"

namespace ui::aircraft_icon {
namespace {

constexpr float kDegToRad = 0.01745329252f;

void normalizeType(const char* in, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  size_t o = 0;
  if (in != nullptr) {
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; ++i) {
      const unsigned char c = static_cast<unsigned char>(in[i]);
      if (c == ' ' || c == '\t') {
        continue;
      }
      out[o++] = static_cast<char>(toupper(c));
    }
  }
  out[o] = '\0';
}

int typeCmpPgm(const char* needle, const char* pgm_type) {
  char buf[5];
  for (int i = 0; i < 4; ++i) {
    buf[i] = static_cast<char>(pgm_read_byte(&pgm_type[i]));
  }
  buf[4] = '\0';
  for (int i = 3; i >= 0; --i) {
    if (buf[i] == ' ' || buf[i] == '\0') {
      buf[i] = '\0';
    } else {
      break;
    }
  }
  return strcmp(needle, buf);
}

bool lookupExactType(const char* type, uint8_t* out_category) {
  if (type == nullptr || type[0] == '\0' || data::aircraft_icons::kTypeMapCount == 0) {
    return false;
  }
  size_t lo = 0;
  size_t hi = data::aircraft_icons::kTypeMapCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const data::aircraft_icons::TypeEntry* entry = &data::aircraft_icons::kTypeMap[mid];
    const int cmp = typeCmpPgm(type, entry->type);
    if (cmp == 0) {
      *out_category = pgm_read_byte(&entry->category);
      return true;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

bool lookupPrefixType(const char* type, uint8_t* out_category) {
  if (type == nullptr || type[0] == '\0') {
    return false;
  }
  const size_t n = strlen(type);
  for (size_t len = n; len >= 3; --len) {
    char prefix[5] = {};
    memcpy(prefix, type, len);
    prefix[len] = '\0';
    if (lookupExactType(prefix, out_category)) {
      return true;
    }
  }
  return false;
}

bool isHelicopterType(const char* type) {
  uint8_t cat = 0;
  if (lookupExactType(type, &cat) &&
      cat == static_cast<uint8_t>(data::aircraft_icons::Category::helicopter)) {
    return true;
  }
  static constexpr const char* kPrefixes[] = {"EC", "AS", "AW", "R4", "R6", "MI",
                                              "KA", "BK", "MD5"};
  for (const char* p : kPrefixes) {
    if (strncmp(type, p, strlen(p)) == 0) {
      return true;
    }
  }
  return false;
}

bool isFighterType(const char* type) {
  static constexpr const char* kPrefixes[] = {
      "F14", "F15", "F16", "F18", "F22", "F35", "F100", "F104", "F111", "F117",
      "EUFI", "RFAL", "HAWK", "TORN", "SU27", "SU30", "SU35", "MIG29", "MIG31",
      "JAS39", "M2K", "M346"};
  for (const char* p : kPrefixes) {
    if (strncmp(type, p, strlen(p)) == 0) {
      return true;
    }
  }
  return false;
}

uint8_t categoryFromAdsbEmitter(const char* cat) {
  if (cat == nullptr || cat[0] == '\0') {
    return data::aircraft_icons::kDefaultCategory;
  }
  using C = data::aircraft_icons::Category;
  if (strcmp(cat, "A7") == 0) {
    return static_cast<uint8_t>(C::helicopter);
  }
  if (strcmp(cat, "B6") == 0) {
    return static_cast<uint8_t>(C::drone);
  }
  if (strcmp(cat, "B1") == 0) {
    return static_cast<uint8_t>(C::glider);
  }
  if (strcmp(cat, "B2") == 0) {
    return static_cast<uint8_t>(C::balloon);
  }
  if (strcmp(cat, "A5") == 0) {
    return static_cast<uint8_t>(C::large_jet_2);
  }
  if (strcmp(cat, "A1") == 0 || strcmp(cat, "B4") == 0) {
    return static_cast<uint8_t>(C::small_prop_single);
  }
  if (strcmp(cat, "A2") == 0) {
    return static_cast<uint8_t>(C::small_prop_twin);
  }
  if (strcmp(cat, "A3") == 0 || strcmp(cat, "A4") == 0) {
    return static_cast<uint8_t>(C::medium_jet);
  }
  if (strcmp(cat, "A6") == 0) {
    return static_cast<uint8_t>(C::military_fighter);
  }
  return data::aircraft_icons::kDefaultCategory;
}

uint8_t militaryFallback(const char* type) {
  using C = data::aircraft_icons::Category;
  if (isFighterType(type)) {
    return static_cast<uint8_t>(C::military_fighter);
  }
  return static_cast<uint8_t>(C::military_transport);
}

}  // namespace

uint8_t resolveCategory(const services::adsb::Aircraft& aircraft) {
  char type[5] = {};
  normalizeType(aircraft.type, type, sizeof(type));

  uint8_t mapped = 0;
  const bool have_map = lookupExactType(type, &mapped) || lookupPrefixType(type, &mapped);
  if (have_map) {
    return mapped;
  }
  if (type[0] != '\0' && isHelicopterType(type)) {
    return static_cast<uint8_t>(data::aircraft_icons::Category::helicopter);
  }
  if (aircraft.isMilitary()) {
    return militaryFallback(type);
  }
  return categoryFromAdsbEmitter(aircraft.category);
}

bool hasIcon(uint8_t category) {
  return category < data::aircraft_icons::kCategoryCount;
}

bool draw(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
          uint8_t category, int base_side_px) {
  if (!hasIcon(category) || base_side_px < 8) {
    return false;
  }

  const data::aircraft_icons::IconBlob* blob = &data::aircraft_icons::kIcons[category];
  const uint8_t* alpha =
      reinterpret_cast<const uint8_t*>(pgm_read_ptr(&blob->alpha));
  const uint16_t scale_x100 = pgm_read_word(&blob->scale_x100);
  const int side =
      std::max(8, static_cast<int>((base_side_px * static_cast<int>(scale_x100) + 50) / 100));

  const float src_side = static_cast<float>(data::aircraft_icons::kIconSide);
  const float src_half = (src_side - 1.0f) * 0.5f;
  const float scale = static_cast<float>(side) / src_side;
  const float inv_scale = 1.0f / scale;

  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);
  const int radius = (side + 1) / 2 + 1;

  // Destination-space sample: every screen pixel is filled (no sparse upscale holes).
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      // Inverse of mapLocal (screen offset → icon-local).
      const float lx = static_cast<float>(dx) * cos_h + static_cast<float>(dy) * sin_h;
      const float ly = -static_cast<float>(dx) * sin_h + static_cast<float>(dy) * cos_h;
      const float ix_f = lx * inv_scale + src_half;
      const float iy_f = ly * inv_scale + src_half;
      const int ix = static_cast<int>(lroundf(ix_f));
      const int iy = static_cast<int>(lroundf(iy_f));
      if (ix < 0 || iy < 0 || ix >= data::aircraft_icons::kIconSide ||
          iy >= data::aircraft_icons::kIconSide) {
        continue;
      }
      const uint8_t a = pgm_read_byte(
          &alpha[static_cast<size_t>(iy) * data::aircraft_icons::kIconSide +
                 static_cast<size_t>(ix)]);
      if (a < data::aircraft_icons::kAlphaFloor) {
        continue;
      }
      gfx.fillRect(static_cast<int16_t>(cx + dx), static_cast<int16_t>(cy + dy), 1, 1, color);
    }
  }
  return true;
}

}  // namespace ui::aircraft_icon
