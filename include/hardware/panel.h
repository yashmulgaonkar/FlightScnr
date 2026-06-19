#pragma once

#include <cstdint>

namespace hardware {

/** T-Encoder Pro display + touch hardware variant. */
enum class PanelType : uint8_t {
  /** DXQ120MYB2416A — SH8601 + CHSC5816 (original). */
  DxqSh8601 = 1,
  /** TFD12MASBCTB4_V0_07 — CO5300 + CST816 (2025+ revision). */
  TfdCo5300 = 2,
};

/**
 * Resolve panel type before displayInit() / inputInit().
 * Order: compile-time force flag → NVS → I2C touch probe → default original panel.
 */
void panelBootResolve();

PanelType panelType();
const char* panelTypeName();
bool panelUsesCo5300();

}  // namespace hardware
