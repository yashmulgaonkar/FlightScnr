#pragma once

#include <cstddef>

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Compose grid + aircraft + sweep in RAM and push one frame to the panel. */
void radarDisplayRefreshSweep();

/** Note ADS-B aircraft changes; panel updates on the next sweep frame. */
void radarDisplayRefreshAircraft();

/** Count of aircraft currently inside the outer ring, i.e. drawn as full
 *  airplanes rather than beyond-ring edge blips. Used by auto-idle so the radar
 *  only reappears once a blip actually enters the visible scope. */
size_t radarDisplayInRangeAircraftCount();

/** Debug probes for [radar] resume diagnostics (kRadarResumeDebug). */
bool radarDisplayDebugBgReady();
bool radarDisplayDebugContentReady();
bool radarDisplayDebugContentBaseValid();

/** Drop offscreen radar sprites to relieve heap pressure (rebuilt on next draw). */
void radarDisplayReleasePressureSprites();

}  // namespace ui
