#pragma once

#include <cstdint>

namespace ui {

void displayPrefsBootLoad();

/** Flight detail auto-return: 0 = manual (swipe away only), else seconds (10/20/30). */
unsigned long displayPrefsFlightDetailTimeoutMs();
const char* displayPrefsFlightDetailTimeoutLabel();
void displayPrefsFlightDetailTimeoutStep(int8_t delta);
void displayPrefsSaveFlightDetailTimeoutFromForm(const char* seconds_str);

bool displayPrefsSweepLineEnabled();
void displayPrefsToggleSweepLine();
void displayPrefsSaveSweepLineFromForm(const char* checkbox_value);

}  // namespace ui
