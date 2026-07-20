#include "services/reboot.h"

#include "hal/wdt_hal.h"

namespace services {

void hardReboot() {
  // Trigger a full chip reset via the RTC watchdog (RWDT). Unlike
  // esp_restart() (RTC_SW_CPU_RST, a CPU/system soft reset that leaves the USB
  // PHY half-initialized), WDT_STAGE_ACTION_RESET_RTC resets the whole chip
  // including the RTC domain — the same class of reset as the hardware button —
  // so USB-CDC-on-boot comes up cleanly with no host attached.
  //
  // API note: arduino-esp32 2.0.x / IDF 4.4 (platform espressif32@6.5.0).
  wdt_hal_context_t rwdt = {.inst = WDT_RWDT, .rwdt_dev = &RTCCNTL};
  wdt_hal_init(&rwdt, WDT_RWDT, 0, false);
  wdt_hal_write_protect_disable(&rwdt);
  // Stage 0: fire almost immediately (1 tick) and reset the whole system + RTC.
  wdt_hal_config_stage(&rwdt, WDT_STAGE0, 1, WDT_STAGE_ACTION_RESET_RTC);
  wdt_hal_enable(&rwdt);
  wdt_hal_write_protect_enable(&rwdt);
  for (;;) {
  }  // wait for the reset
}

}  // namespace services
