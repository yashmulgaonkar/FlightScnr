#pragma once

#include "hardware/plane_gfx.h"

extern PlaneGfx tft;

void displayInit();
void planeGfxPanelLockInit();

/** Put the panel into sleep mode (display off, low power). */
void displaySleep();
/** Wake the panel from sleep mode. */
void displayWake();
