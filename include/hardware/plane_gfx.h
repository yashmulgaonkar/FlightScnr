#pragma once

#include <Arduino_GFX_Library.h>
#include <cstdint>

#include "hardware/aa_font.h"

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
  /** Optional RGB565 framebuffer for AA text blending (sprites / offscreen). */
  void setFramebuffer(uint16_t* buf, int16_t w, int16_t h) {
    fb_ = buf;
    fb_w_ = w;
    fb_h_ = h;
  }
  Arduino_GFX* raw() const { return gfx_; }

  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  /** Filled annulus sector. Angles: 0° = 3 o'clock, clockwise (Arduino_GFX). */
  void fillArc(int16_t x, int16_t y, int16_t r_outer, int16_t r_inner, float start_deg,
               float end_deg, uint16_t color);
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
  void setFont(const AaFont* font);
  void setTextDatum(TextDatum datum);
  void setTextWrap(bool wrap);

  int textWidth(const char* text) const;
  int fontHeight() const;
  void drawString(const char* text, int16_t x, int16_t y);

  void startWrite();
  void endWrite();

  /** Redirect all drawing into a full-screen RAM canvas, then blit it to the
   *  panel in one aligned pass on endOffscreen(). Lets 1-bit glyphs render at
   *  full resolution on panels that quantize direct pixel writes to a 2x2 grid
   *  (CO5300 / pixelAlign2). No-op returning false when pixelAlign2 is off, a
   *  panel write session is open, or the canvas allocation fails. */
  bool beginOffscreen();
  void endOffscreen();
  /**
   * Finish an offscreen compose but flush only one region. The cached canvas
   * retains the rest of the previous frame, allowing small dynamic labels to
   * update without a full-screen redraw.
   */
  void endOffscreenRegion(int16_t x, int16_t y, int16_t w, int16_t h);

  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap, int16_t w,
                          int16_t h);
  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap,
                          uint16_t transparent_color, int16_t w, int16_t h);
  /** Copies a screen region to the hardware panel. */
  void blitRegionFromBuffer(int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint16_t* src, int16_t src_stride);

 private:
  Arduino_GFX* gfx_ = nullptr;
  bool hardware_panel_ = false;
  TextDatum datum_ = TextDatum::TopLeft;
  uint8_t write_depth_ = 0;

  const AaFont* aa_font_ = nullptr;
  uint16_t text_fg_ = 0xFFFF;
  uint16_t text_bg_ = 0xFFFF;
  bool text_transparent_ = true;

  uint16_t* fb_ = nullptr;
  int16_t fb_w_ = 0;
  int16_t fb_h_ = 0;

  // Offscreen compose state (see beginOffscreen): a cached full-screen canvas
  // that temporarily replaces gfx_ so draws bypass the panel's 2x2 alignment.
  Arduino_GFX* offscreen_canvas_ = nullptr;
  uint16_t* offscreen_buf_ = nullptr;
  Arduino_GFX* saved_gfx_ = nullptr;
  bool saved_hardware_panel_ = false;
  bool offscreen_active_ = false;

  bool targetUsesPixelAlign2() const;
  void drawLinePixelAlign2(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                           uint16_t color);
  /** Single SPI flush to the hardware panel. */
  void panelFlushBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                        const uint16_t* src);
  void drawLineInternal(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint16_t color);
  void mapDatum(const char* text, int16_t x, int16_t y, int16_t* out_x,
                int16_t* out_y) const;
  void getAaTextBounds(const char* text, int16_t x, int16_t y, int16_t* x1,
                       int16_t* y1, uint16_t* w, uint16_t* h) const;
  void drawAaString(const char* text, int16_t cursor_x, int16_t cursor_y);
};

/** RAII hardware panel SPI session for one composited frame. */
class PanelSession {
 public:
  explicit PanelSession(PlaneGfx& gfx) : gfx_(&gfx) { gfx_->startWrite(); }
  ~PanelSession() { gfx_->endWrite(); }

  PanelSession(const PanelSession&) = delete;
  PanelSession& operator=(const PanelSession&) = delete;

 private:
  PlaneGfx* gfx_;
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
