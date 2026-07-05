#pragma once

#include <cstdint>

namespace ui::radar {

constexpr int kSize = 390;
constexpr int kCenterX = kSize / 2;
constexpr int kCenterY = kSize / 2;

/** Outermost grid ring (inside edge labels). */
constexpr int kGridOuterRadius = 174;

/** N: pixels inset from top edge (TopCenter anchor). */
constexpr int kCardinalNorthOffsetY = 10;
/** S: pixels inset from bottom edge (BottomCenter anchor). */
constexpr int kCardinalSouthOffsetY = 10;
/** Intercardinal (NE/SE/SW/NW) labels sit on the outer grid ring minus this inset. */
constexpr int kCardinalDiagonalInsetPx = 6;

/** Radial inset from each ring for scale labels (px; positive = toward center). */
constexpr int kScaleGapFromOuterRing = 12;
/** Outermost ring only, when distance units are km. */
constexpr int kScaleGapOuterRingKm = 20;
/** Range ring labels: bearing from north, clockwise (between W and SW on display). */
constexpr float kScaleLabelBearingDeg = 245.5f;

/** Target cap height (px) for N/S/E/W. */
constexpr int kCardinalLabelHeightPx = 23;
/** Scale label is this many px shorter than cardinals. */
constexpr int kScaleBelowCardinalPx = 5;

constexpr int kRingCount = 3;

/** Shared grid stroke: drawWideLine half-width (~2 px total); rings use the same px count. */
constexpr float kGridStrokeHalfWidth = 1.0f;
/** Dashed grid: dash and gap length along lines / arcs (px). */
constexpr int kGridDashLenPx = 7;
constexpr int kGridDashGapPx = 15;

/** Top-down aircraft icon half-extent (see ui/aircraft_symbol). */
constexpr int kAircraftIconRadiusPx = 15;
/** Gap from icon edge to tag block (px). */
constexpr int kAircraftLabelGapPx = 3;
/** Keep icon inside outer ring by at least this inset (px). */
constexpr int kAircraftInsideRingInsetPx = kAircraftIconRadiusPx + 2;

/** Beyond-ring traffic: compact aircraft icons on screen rim. */
constexpr int kBeyondRingScreenMarginPx = 3;
/** Target cap height (px) for aircraft tags (bold, slightly above scale label). */
constexpr int kAircraftTagLabelHeightPx = 21;

/** Radar sweep: one full rotation period (ms). */
constexpr unsigned long kSweepPeriodMs = 6000;
/** Target animation frame interval (ms); partial spoke blits are ~15–25ms. */
constexpr unsigned long kSweepFrameMs = 33;
/** Loop gap (ms) after which the sweep pauses instead of catching up to wall clock. */
constexpr unsigned long kSweepGapPauseMs = 80;
/** Max simulated dt (ms) per painted frame (~2.9 deg at kSweepPeriodMs). */
constexpr unsigned long kSweepMaxStepMs = 48;
/** Spoke erase uses incremental union only when angle moved less than this (deg). */
constexpr float kSweepIncrementalMaxDeg = 10.0f;
/** Sweep line from center to this radius (px). */
constexpr int kSweepRadiusPx = kCenterX - kBeyondRingScreenMarginPx;
/** drawWideLine half-width for the sweep spoke (~2 px total). */
constexpr float kSweepLineHalfWidth = 1.0f;
/** Trailing sweep lines (1 = leading spoke only, no trail). */
constexpr int kSweepTrailLines = 1;
constexpr float kSweepTrailSpanDeg = 12.0f;
/** Chroma key for sweep sprite (must not appear in grid or sweep colors). */
constexpr uint16_t kSweepTransparentColor = 0x0001;

/** RGB565 palette targets (applied in initPalette). */
constexpr uint8_t kBgR = 2;
constexpr uint8_t kBgG = 15;
constexpr uint8_t kBgB = 3;
constexpr uint8_t kGridR = 16;
constexpr uint8_t kGridG = 100;
constexpr uint8_t kGridB = 32;
constexpr uint8_t kSweepR = 48;
constexpr uint8_t kSweepG = 255;
constexpr uint8_t kSweepB = 96;
constexpr uint8_t kSweepTrailR = 12;
constexpr uint8_t kSweepTrailG = 72;
constexpr uint8_t kSweepTrailB = 28;
/** Aircraft icon fill (warm amber). */
constexpr uint8_t kAircraftR = 255;
constexpr uint8_t kAircraftG = 180;
constexpr uint8_t kAircraftB = 40;
constexpr uint8_t kTagTypeR = 255;
constexpr uint8_t kTagTypeG = 200;
constexpr uint8_t kTagTypeB = 0;
/** Altitude tag: ascending or level (cyan). */
constexpr uint8_t kTagAltAscendR = 0;
constexpr uint8_t kTagAltAscendG = 255;
constexpr uint8_t kTagAltAscendB = 255;
/** Altitude tag: descending (magenta). */
constexpr uint8_t kTagAltDescendR = 255;
constexpr uint8_t kTagAltDescendG = 0;
constexpr uint8_t kTagAltDescendB = 255;

extern uint16_t kColorBackground;
extern uint16_t kColorGrid;
extern uint16_t kColorSweep;
extern uint16_t kColorSweepTrail;
extern uint16_t kColorLabel;
extern uint16_t kColorAircraft;
extern uint16_t kColorTagType;
extern uint16_t kColorTagAltitudeAscend;
extern uint16_t kColorTagAltitudeDescend;
extern uint16_t kColorAlertMilitary;
extern uint16_t kColorAlertEmergency;

}  // namespace ui::radar
