#pragma once

#include <Arduino_GFX_Library.h>
#include <cstdint>

enum class TextDatum : uint8_t {
  TopLeft,
  TopCenter,
  TopRight,
  MiddleLeft,
  MiddleCenter,
  MiddleRight,
  BottomLeft,
  BottomCenter,
  BottomRight,
};

/** Drawing helpers and text layout on top of Arduino_GFX (SH8601). */
class PlaneGfx {
 public:
  PlaneGfx() = default;

  void attach(Arduino_GFX* gfx, bool hardware_panel = false) {
    gfx_ = gfx;
    hardware_panel_ = hardware_panel;
  }
  Arduino_GFX* raw() const { return gfx_; }

  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2,
                    int16_t y2, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void drawWideLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, float half_width,
                    uint16_t color);

  uint16_t color565(uint8_t r, uint8_t g, uint8_t b) const;

  void setTextColor(uint16_t fg);
  void setTextColor(uint16_t fg, uint16_t bg);
  void setTextSize(uint8_t size);
  void setFont(const GFXfont* font);
  void setTextDatum(TextDatum datum);
  void setTextWrap(bool wrap);

  int textWidth(const char* text) const;
  int fontHeight() const;
  void drawString(const char* text, int16_t x, int16_t y);

  void startWrite();
  void endWrite();

  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap, int16_t w,
                          int16_t h);
  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap,
                          uint16_t transparent_color, int16_t w, int16_t h);
  /** Copies a screen region; caller must hold startWrite() on the target display. */
  void blitRegionFromBuffer(int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint16_t* src, int16_t src_stride);

 private:
  Arduino_GFX* gfx_ = nullptr;
  bool hardware_panel_ = false;
  TextDatum datum_ = TextDatum::TopLeft;
  bool write_open_ = false;

  bool targetUsesPixelAlign2() const;
  void drawLinePixelAlign2(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                           uint16_t color);
  /** Single SPI flush; skips nested startWrite when a batch is already open. */
  void panelFlushBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                        const uint16_t* src);
  void mapDatum(const char* text, int16_t x, int16_t y, int16_t* out_x,
                int16_t* out_y) const;
};

/** Off-screen buffer for static radar grid (uses PSRAM when available). */
class PlaneGfxSprite {
 public:
  explicit PlaneGfxSprite(PlaneGfx* parent);
  ~PlaneGfxSprite();

  bool createSprite(int16_t w, int16_t h);
  void deleteSprite();
  bool ready() const { return buffer_ != nullptr; }
  int16_t width() const { return width_; }
  int16_t height() const { return height_; }
  const uint16_t* buffer() const { return buffer_; }
  uint16_t* bufferMut() { return buffer_; }

  PlaneGfx& gfx() { return canvas_; }
  void pushSprite(int16_t x, int16_t y);
  /** Blit sprite skipping pixels equal to transparent_color (non-blocking overlay). */
  void pushSprite(int16_t x, int16_t y, uint16_t transparent_color);

 private:
  PlaneGfx* parent_ = nullptr;
  PlaneGfx canvas_;
  uint16_t* buffer_ = nullptr;
  int16_t width_ = 0;
  int16_t height_ = 0;
};
