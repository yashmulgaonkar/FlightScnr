#pragma once

#include <cstdint>

/**
 * Draw a centered black-on-white Wi‑Fi join QR for an open SoftAP.
 * Payload: WIFI:T:nopass;S:<ssid>;;
 * Returns false if encode/draw failed (caller can fall back to text-only).
 */
bool drawWifiJoinQr(int center_x, int center_y, int module_px, const char* ssid);
