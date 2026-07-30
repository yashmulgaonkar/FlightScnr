#include "ui/qr_draw.h"

#include <cstdio>
#include <cstring>

#include <qrcode.h>

#include "config.h"
#include "hardware/display.h"

namespace {

/** Version 3 (29×29): enough for WIFI:nopass + FlightScnr-AP-XXXX. */
constexpr uint8_t kQrVersion = 3;
constexpr uint8_t kQuietModules = 4;

}  // namespace

bool drawWifiJoinQr(int center_x, int center_y, int module_px, const char* ssid) {
  if (ssid == nullptr || ssid[0] == '\0' || module_px < 1) {
    return false;
  }

  char payload[64];
  const int n =
      snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(payload)) {
    return false;
  }

  QRCode qr;
  // Version 3 → ((29*29)+7)/8 = 106 bytes; pad for safety.
  uint8_t modules[128];
  if (sizeof(modules) < qrcode_getBufferSize(kQrVersion)) {
    return false;
  }
  if (qrcode_initText(&qr, modules, kQrVersion, ECC_MEDIUM, payload) != 0) {
    return false;
  }

  const int code_px = static_cast<int>(qr.size) * module_px;
  const int quiet_px = static_cast<int>(kQuietModules) * module_px;
  const int box = code_px + 2 * quiet_px;
  const int box_x = center_x - box / 2;
  const int box_y = center_y - box / 2;

  tft.fillRect(static_cast<int16_t>(box_x), static_cast<int16_t>(box_y),
               static_cast<int16_t>(box), static_cast<int16_t>(box),
               config::kTextOnBlack);

  const int code_x = box_x + quiet_px;
  const int code_y = box_y + quiet_px;
  for (uint8_t y = 0; y < qr.size; ++y) {
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (!qrcode_getModule(&qr, x, y)) {
        continue;
      }
      tft.fillRect(static_cast<int16_t>(code_x + static_cast<int>(x) * module_px),
                   static_cast<int16_t>(code_y + static_cast<int>(y) * module_px),
                   static_cast<int16_t>(module_px), static_cast<int16_t>(module_px),
                   config::kColorBlack);
    }
  }
  return true;
}
