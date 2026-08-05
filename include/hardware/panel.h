#pragma once

#include <cstdint>

namespace hardware {

/** Display + touch hardware variant (board + panel revision). */
enum class PanelType : uint8_t {
  /** DXQ120MYB2416A — SH8601 + CHSC5816 (T-Encoder Pro original). */
  DxqSh8601 = 1,
  /** TFD12MASBCTB4_V0_07 — CO5300 + CST816 (T-Encoder Pro 2025+). */
  TfdCo5300 = 2,
  /** Waveshare ESP32-S3-Knob-Touch-LCD-1.8 — ST77916 + CST816. */
  WaveshareSt77916 = 3,
};

/**
 * Resolve panel type before displayInit() / inputInit().
 * T-Encoder: compile-time force flag → NVS → I2C touch probe → default original.
 * Waveshare: fixed at compile time.
 */
void panelBootResolve();

PanelType panelType();
const char* panelTypeName();
bool panelUsesCo5300();
/** True when the board uses CST816 (TFD12 or Waveshare). */
bool panelUsesCst816();

}  // namespace hardware
