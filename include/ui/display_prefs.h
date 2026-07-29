#pragma once

#include <cstdint>

namespace ui {

void displayPrefsBootLoad();

/** Flight detail auto-return: 0 = manual (swipe away only), else seconds (10/20/30). */
unsigned long displayPrefsFlightDetailTimeoutMs();
const char* displayPrefsFlightDetailTimeoutLabel();
void displayPrefsFlightDetailTimeoutStep(int8_t delta);
void displayPrefsSaveFlightDetailTimeoutFromForm(const char* seconds_str);

/** Clock/forecast auto-return: 0 = manual (swipe away only), else seconds (5/10/15). */
unsigned long displayPrefsClockWeatherTimeoutMs();
const char* displayPrefsClockWeatherTimeoutLabel();
void displayPrefsClockWeatherTimeoutStep(int8_t delta);
void displayPrefsSaveClockWeatherTimeoutFromForm(const char* seconds_str);

bool displayPrefsSweepLineEnabled();
void displayPrefsToggleSweepLine();
void displayPrefsSaveSweepLineFromForm(const char* checkbox_value);

/** When true, radar draws plane icons only (no callsign / type / altitude tags). */
bool displayPrefsHideBlipDetails();
void displayPrefsToggleHideBlipDetails();
void displayPrefsSaveHideBlipDetailsFromForm(const char* checkbox_value);

/** When true, an empty radar (no in-range aircraft) auto-opens the clock screen. */
bool displayPrefsAutoIdleClockEnabled();
void displayPrefsToggleAutoIdleClock();
void displayPrefsSaveAutoIdleClockFromForm(const char* checkbox_value);

}  // namespace ui
