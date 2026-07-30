#pragma once

namespace services::device_identity {

/**
 * Build SoftAP / mDNS names from the chip MAC (last 2 bytes).
 * Safe to call more than once; accessors also lazy-init.
 */
void init();

/** Last 2 MAC bytes as uppercase hex, e.g. "A1B2". */
const char* idSuffix();

/** SoftAP SSID, e.g. "FlightScnr-AP-A1B2". */
const char* portalApName();

/** mDNS host without ".local", e.g. "flightscnr-a1b2". */
const char* portalHostname();

/** Browser host, e.g. "flightscnr-a1b2.local". */
const char* portalHostUrl();

}  // namespace services::device_identity
