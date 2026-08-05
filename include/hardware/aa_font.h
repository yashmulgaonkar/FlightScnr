#pragma once

#include <cstdint>

/** 4-bit alpha glyph (GFXfont-compatible metrics). Bitmaps pack high nibble first. */
struct AaGlyph {
  uint32_t bitmapOffset;
  uint8_t width;
  uint8_t height;
  uint8_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
};

struct AaFont {
  const uint8_t* bitmap;
  const AaGlyph* glyph;
  uint8_t first;
  uint8_t last;
  uint8_t yAdvance;
};
