#pragma once

#include <cstdint>

namespace ui {

enum class InfoSettingsPage : uint8_t { Main, Display, Colors };

void infoScreenResetToMain();
InfoSettingsPage infoScreenPage();
void infoScreenSetPage(InfoSettingsPage page);

/** Settings / status screens (three pages). */
void infoScreenDraw();

/** Rotate knob on display/colors page adjusts the highlighted row. */
void infoScreenHandleKnob(int8_t delta);

/** Short knob press on display page cycles highlighted setting row. */
void infoScreenCycleDisplayFocus();

/** True when Display page focus is Facing (dial opens orientation adjust). */
bool infoScreenFacingFocused();

/** Default display-page selection when opening page 2. */
void infoScreenResetDisplayFocus();

/** Short knob press on page 3/3 cycles highlighted setting row. */
void infoScreenCycleColorsFocus();

/** Default page-3 selection when opening Colors. */
void infoScreenResetColorsFocus();

}  // namespace ui
