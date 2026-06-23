#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

/** Let the prior TLS session release buffers before opening another connection. */
inline void drainTlsHeapAfterSession(uint32_t timeout_ms = 800) {
  vTaskDelay(pdMS_TO_TICKS(150));
  const unsigned long started = millis();
  const unsigned long deadline = started + timeout_ms;
  while (millis() < deadline) {
    const uint32_t blk = ESP.getMaxAllocHeap();
    if (blk >= config::kMinContiguousHeapForTlsReconnect) {
      return;
    }
    if (blk >= config::kMinContiguousHeapForAdsbTls && millis() - started >= 350) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

/** @deprecated Use heapReadyForAdsb or heapReadyForRouteApi. */
inline bool heapReady() { return heapReadyForRouteApi(); }

}  // namespace services::https
