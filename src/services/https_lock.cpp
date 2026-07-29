#include "services/https_lock.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "mbedtls/platform.h"

namespace services::https {

namespace {

SemaphoreHandle_t s_sem = nullptr;
TaskHandle_t s_holder = nullptr;
bool s_tls_psram_alloc_set = false;

void ensureSem() {
  if (s_sem != nullptr) {
    return;
  }
  // Binary semaphore (not a mutex): force-unlock after vTaskDelete must Give from
  // a different task. FreeRTOS mutexes assert if a non-owner Gives.
  s_sem = xSemaphoreCreateBinary();
  if (s_sem != nullptr) {
    xSemaphoreGive(s_sem);  // start available
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
  ensureSem();
  ensureTlsPsramAllocator();
}

bool lock(uint32_t timeout_ms) {
  ensureSem();
  if (s_sem == nullptr) {
    return false;
  }
  if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return false;
  }
  s_holder = xTaskGetCurrentTaskHandle();
  return true;
}

void unlock() {
  if (s_sem == nullptr) {
    return;
  }
  if (s_holder == xTaskGetCurrentTaskHandle()) {
    s_holder = nullptr;
  }
  xSemaphoreGive(s_sem);
}

bool heldBy(TaskHandle_t task) {
  return task != nullptr && s_holder == task;
}

void forceUnlockIfHeldBy(TaskHandle_t task) {
  ensureSem();
  if (s_sem == nullptr || task == nullptr) {
    return;
  }
  if (s_holder != task) {
    return;
  }
  s_holder = nullptr;
  // Binary Give from any task is safe; ignore errQUEUE_FULL if already free.
  xSemaphoreGive(s_sem);
}

void forceUnlock() {
  ensureSem();
  if (s_sem == nullptr) {
    return;
  }
  s_holder = nullptr;
  xSemaphoreGive(s_sem);
}

bool busy() {
  ensureSem();
  if (s_sem == nullptr) {
    return false;
  }
  if (xSemaphoreTake(s_sem, 0) == pdTRUE) {
    s_holder = nullptr;
    xSemaphoreGive(s_sem);
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
