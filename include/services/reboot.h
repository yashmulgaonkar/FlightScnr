#pragma once

namespace services {

/**
 * Perform a full chip reset equivalent to the hardware reset button.
 *
 * Does NOT return.
 *
 * esp_restart() only issues a CPU/system soft reset (RTC_SW_CPU_RST). On this
 * board (ARDUINO_USB_CDC_ON_BOOT=1, qio_opi), a soft reset does not fully
 * reset the USB peripheral/PHY, so the freshly booted firmware hangs in early
 * USB-CDC init unless a USB host is attached. A full chip reset re-initializes
 * the USB PHY, matching the behavior of the physical reset button, so the
 * device comes up cleanly with no host connected.
 *
 * Use this for any unattended reboot in the field (OTA, heap recovery, Wi-Fi
 * reconfigure) rather than esp_restart()/ESP.restart().
 */
[[noreturn]] void hardReboot();

}  // namespace services
