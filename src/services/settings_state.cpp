#include "services/settings_state.h"

#include <Arduino.h>

namespace {

uint32_t s_rev = 1;
unsigned long s_updated_ms = 0;

}  // namespace

uint32_t settingsStateRev() { return s_rev; }

unsigned long settingsStateUpdatedMs() { return s_updated_ms; }

void settingsStateBump() {
  ++s_rev;
  if (s_rev == 0) {
    s_rev = 1;
  }
  s_updated_ms = millis();
}
