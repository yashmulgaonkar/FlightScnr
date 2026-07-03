#pragma once

#include <cstdint>

namespace services::offhours {

enum class Mode : uint8_t {
  Dim = 0,
  DisplayOff = 1,
};

void bootLoad();

/** True when off-hours is enabled AND current local time is inside the window. */
bool active();

/** Current configured mode (only meaningful when active). */
Mode mode();

bool enabled();
uint16_t startMinute();
uint16_t endMinute();

void saveFromForm(const char* enable_checkbox, const char* mode_str,
                  const char* start_str, const char* end_str);

}  // namespace services::offhours
