#include "services/airline_logo.h"

#include <AnimatedGIF.h>
#include <Arduino.h>
#include <cstring>

#include "data/airline_logos_lookup.h"
#include "hardware/plane_gfx.h"

namespace services::airline {

namespace {

AnimatedGIF s_gif;
uint16_t* s_frame = nullptr;
int s_frame_w = 0;
int s_frame_h = 0;
bool s_gif_ready = false;

int compareIcao(const char* key, const data::airline_logos::Entry* entry) {
  for (int i = 0; i < 3; ++i) {
    const char e = static_cast<char>(pgm_read_byte(&entry->icao[i]));
    if (key[i] != e) {
      return (key[i] < e) ? -1 : 1;
    }
  }
  return 0;
}

const data::airline_logos::LogoBlob* findLogoBlob(const char* icao) {
  if (icao == nullptr || strlen(icao) != 3) {
    return nullptr;
  }
  char key[4];
  for (int i = 0; i < 3; ++i) {
    key[i] = static_cast<char>(toupper(static_cast<unsigned char>(icao[i])));
  }
  key[3] = '\0';

  size_t lo = 0;
  size_t hi = data::airline_logos::kCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const data::airline_logos::Entry* entry =
        &data::airline_logos::kEntries[mid];
    const int cmp = compareIcao(key, entry);
    if (cmp == 0) {
      return reinterpret_cast<const data::airline_logos::LogoBlob*>(
          pgm_read_ptr(&entry->logo));
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return nullptr;
}

bool ensureFrameBuffer(int w, int h) {
  if (w <= 0 || h <= 0 || w > data::airline_logos::kMaxLogoW ||
      h > data::airline_logos::kMaxLogoH) {
    return false;
  }
  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (s_frame != nullptr && s_frame_w == w && s_frame_h == h) {
    return true;
  }
  if (s_frame != nullptr) {
    free(s_frame);
    s_frame = nullptr;
  }
  s_frame = static_cast<uint16_t*>(ps_malloc(pixels * sizeof(uint16_t)));
  if (s_frame == nullptr) {
    s_frame = static_cast<uint16_t*>(malloc(pixels * sizeof(uint16_t)));
  }
  if (s_frame == nullptr) {
    return false;
  }
  s_frame_w = w;
  s_frame_h = h;
  return true;
}

void GIFDraw(GIFDRAW* p_draw) {
  if (s_frame == nullptr || p_draw == nullptr) {
    return;
  }
  const int y = p_draw->y;
  if (y < 0 || y >= s_frame_h) {
    return;
  }
  uint8_t* pixels = p_draw->pPixels;
  uint16_t* palette = p_draw->pPalette;
  uint16_t* row = s_frame + y * s_frame_w;
  const int width = p_draw->iWidth;
  if (p_draw->ucHasTransparency) {
    const uint8_t transparent = p_draw->ucTransparent;
    for (int i = 0; i < width; ++i) {
      row[i] = (pixels[i] == transparent) ? data::airline_logos::kTransparentRgb565
                                          : palette[pixels[i]];
    }
  } else {
    for (int i = 0; i < width; ++i) {
      row[i] = palette[pixels[i]];
    }
  }
}

bool decodeGifToFrame(const data::airline_logos::LogoBlob* blob) {
  if (blob == nullptr) {
    return false;
  }
  const uint16_t w = pgm_read_word(&blob->w);
  const uint16_t h = pgm_read_word(&blob->h);
  const uint32_t size = pgm_read_dword(&blob->size);
  const uint8_t* gif = reinterpret_cast<const uint8_t*>(pgm_read_ptr(&blob->gif));
  if (gif == nullptr || size == 0 || w == 0 || h == 0) {
    return false;
  }
  if (!ensureFrameBuffer(w, h)) {
    return false;
  }
  for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) {
    s_frame[i] = data::airline_logos::kTransparentRgb565;
  }

  if (!s_gif_ready) {
    s_gif.begin(LITTLE_ENDIAN_PIXELS);
    s_gif_ready = true;
  }

  if (!s_gif.open(const_cast<uint8_t*>(gif), static_cast<int>(size), GIFDraw)) {
    return false;
  }
  s_gif.playFrame(true, nullptr);
  s_gif.close();
  return true;
}

}  // namespace

int logoHeightForIcao(const char* icao) {
  const data::airline_logos::LogoBlob* blob = findLogoBlob(icao);
  if (blob == nullptr) {
    return 0;
  }
  return static_cast<int>(pgm_read_word(&blob->h));
}

bool drawLogo(PlaneGfx& tft, const char* icao, int16_t center_x, int16_t y,
              uint16_t bg) {
  const data::airline_logos::LogoBlob* blob = findLogoBlob(icao);
  if (blob == nullptr) {
    return false;
  }
  if (!decodeGifToFrame(blob)) {
    return false;
  }
  const int w = s_frame_w;
  const int h = s_frame_h;
  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  const uint16_t key = data::airline_logos::kTransparentRgb565;
  for (size_t i = 0; i < pixels; ++i) {
    if (s_frame[i] == key) {
      s_frame[i] = bg;
    }
  }
  const int16_t x = static_cast<int16_t>(center_x - w / 2);
  tft.draw16bitRGBBitmap(x, y, s_frame, static_cast<int16_t>(w), static_cast<int16_t>(h));
  return true;
}

void releaseLogoBuffer() {
  if (s_frame != nullptr) {
    free(s_frame);
    s_frame = nullptr;
  }
  s_frame_w = 0;
  s_frame_h = 0;
}

}  // namespace services::airline
