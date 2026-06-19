#include "hardware/panel.h"

#include <Preferences.h>
#include <Wire.h>

#include <Arduino.h>

#include "hardware/pin_config.h"

namespace hardware {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kPanelKey[] = "panel_hw";

PanelType s_panel = PanelType::DxqSh8601;

bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void pulseTouchReset() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, HIGH);
  delay(10);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(50);
}

PanelType probeTouchPanel() {
  Wire.begin(IIC_SDA, IIC_SCL);
  pulseTouchReset();

  const bool cst816 = i2cDevicePresent(CST816_SLAVE_ADDRESS);
  const bool chsc5816 = i2cDevicePresent(CHSC5816_SLAVE_ADDRESS);

  if (cst816 && !chsc5816) {
    return PanelType::TfdCo5300;
  }
  if (chsc5816 && !cst816) {
    return PanelType::DxqSh8601;
  }
  if (cst816 && chsc5816) {
    Serial.println("Panel: both touch ICs seen — using SH8601 / CHSC5816");
    return PanelType::DxqSh8601;
  }
  Serial.println("Panel: no touch IC detected — defaulting to SH8601 / CHSC5816");
  return PanelType::DxqSh8601;
}

bool readStoredPanel(PanelType* out) {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return false;
  }
  const uint8_t stored = prefs.getUChar(kPanelKey, 0);
  prefs.end();
  if (stored == static_cast<uint8_t>(PanelType::DxqSh8601) ||
      stored == static_cast<uint8_t>(PanelType::TfdCo5300)) {
    *out = static_cast<PanelType>(stored);
    return true;
  }
  return false;
}

void persistPanel(PanelType type) {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kPanelKey, static_cast<uint8_t>(type));
    prefs.end();
  }
}

}  // namespace

void panelBootResolve() {
#if defined(FLIGHTSCNR_PANEL_TFD12)
  s_panel = PanelType::TfdCo5300;
  Serial.println("Panel: forced TFD12 / CO5300 / CST816 (build flag)");
  return;
#elif defined(FLIGHTSCNR_PANEL_DXQ)
  s_panel = PanelType::DxqSh8601;
  Serial.println("Panel: forced DXQ120 / SH8601 / CHSC5816 (build flag)");
  return;
#endif

  PanelType stored = PanelType::DxqSh8601;
  if (readStoredPanel(&stored)) {
    s_panel = stored;
    Serial.printf("Panel: %s (saved)\n", panelTypeName());
    return;
  }

  s_panel = probeTouchPanel();
  persistPanel(s_panel);
  Serial.printf("Panel: %s (auto-detected, saved)\n", panelTypeName());
}

PanelType panelType() { return s_panel; }

const char* panelTypeName() {
  switch (s_panel) {
    case PanelType::TfdCo5300:
      return "TFD12 / CO5300";
    case PanelType::DxqSh8601:
    default:
      return "DXQ120 / SH8601";
  }
}

bool panelUsesCo5300() { return s_panel == PanelType::TfdCo5300; }

}  // namespace hardware
