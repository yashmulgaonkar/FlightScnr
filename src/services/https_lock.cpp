#include "services/https_lock.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "mbedtls/platform.h"

namespace services::https {

namespace {

SemaphoreHandle_t s_mutex = nullptr;
bool s_tls_psram_alloc_set = false;

void ensureMutex() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

// mbedTLS keeps its handshake/record buffers in internal RAM by default. On this
// board the heap fragments after the first TLS session (max_blk stuck ~36 KB),
// so a server with a larger certificate chain (e.g. api.tomorrow.io) can't get a
// big enough contiguous internal block and fails with -32512. Steering mbedTLS
// allocations to PSRAM (with an internal fallback) frees internal heap for every
// HTTPS user — ADS-B, route lookups, and weather alike.
void* tlsCallocPsram(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) {
    p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return p;
}

void tlsFree(void* p) { heap_caps_free(p); }

void ensureTlsPsramAllocator() {
  if (s_tls_psram_alloc_set) {
    return;
  }
  // No-op (returns 0) when PSRAM is unavailable / allocator already fixed.
  mbedtls_platform_set_calloc_free(tlsCallocPsram, tlsFree);
  s_tls_psram_alloc_set = true;
}

}  // namespace

void init() {
  ensureMutex();
  ensureTlsPsramAllocator();
}

bool lock(uint32_t timeout_ms) {
  ensureMutex();
  if (s_mutex == nullptr) {
    return false;
  }
  return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void unlock() {
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void forceUnlock() {
  ensureMutex();
  if (s_mutex == nullptr) {
    return;
  }
  // Mutex may already be free; Give on an untaken mutex is undefined on some
  // ports — only Give when Take(0) fails (still held by a dead task).
  if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
    xSemaphoreGive(s_mutex);
    return;
  }
  xSemaphoreGive(s_mutex);
}

bool busy() {
  ensureMutex();
  if (s_mutex == nullptr) {
    return false;
  }
  if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
    xSemaphoreGive(s_mutex);
    return false;
  }
  return true;
}

ScopedLock::ScopedLock(uint32_t timeout_ms) {
  held_ = lock(timeout_ms);
}

ScopedLock::~ScopedLock() {
  if (held_) {
    unlock();
  }
}

}  // namespace services::https
