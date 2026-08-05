#pragma once

#include <cstdint>

namespace ui {

/**
 * Reset checkbox state for a new disclaimer presentation.
 * @param remember_checked initial "Don't show again" state (true when NVS matches).
 */
void disclaimerScreenReset(bool remember_checked);

/**
 * Full-screen round disclaimer with Accept and "Don't show again" controls.
 * @param countdown_sec remaining auto-continue seconds, or -1 when waiting for Accept.
 */
void disclaimerScreenDraw(int countdown_sec = -1);

/** Update only the remembered-boot countdown label (no full-screen redraw). */
void disclaimerScreenUpdateCountdown(int countdown_sec);

/** True if (x,y) is inside the Accept button hit target. */
bool disclaimerScreenHitAccept(int16_t x, int16_t y);

/** True if (x,y) is inside the "Don't show again" checkbox hit target. */
bool disclaimerScreenHitRemember(int16_t x, int16_t y);

bool disclaimerScreenRememberChecked();

void disclaimerScreenSetRememberChecked(bool checked);

void disclaimerScreenToggleRemember();

}  // namespace ui
