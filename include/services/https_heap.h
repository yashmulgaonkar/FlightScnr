#pragma once

#include <Arduino.h>

#include "config.h"

namespace services::https {

/** ADS-B poll (~2s JSON) — lower bar than route APIs. */
inline bool heapReadyForAdsb() {
  return ESP.getFreeHeap() >= config::kMinFreeHeapForAdsbHttps &&
         ESP.getMaxAllocHeap() >= config::kMinContiguousHeapForAdsbTls;
}

/** FlightAware / AirLabs / FR24 detail enrichment. */
inline bool heapReadyForRouteApi() {
  return ESP.getFreeHeap() >= config::kMinFreeHeapForRouteHttps &&
         ESP.getMaxAllocHeap() >= config::kMinContiguousHeapForRouteTls;
}

/** @deprecated Use heapReadyForAdsb or heapReadyForRouteApi. */
inline bool heapReady() { return heapReadyForRouteApi(); }

}  // namespace services::https
