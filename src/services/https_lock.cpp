#include "services/https_lock.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "mbedtls/platform.h"

namespace services::https {

namespace {

SemaphoreHandle_t s_sem = nullptr;
TaskHandle_t s_holder = nullptr;
portMUX_TYPE s_holder_mux = portMUX_INITIALIZER_UNLOCKED;
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
  portENTER_CRITICAL(&s_holder_mux);
  s_holder = xTaskGetCurrentTaskHandle();
  portEXIT_CRITICAL(&s_holder_mux);
  return true;
}

void unlock() {
  if (s_sem == nullptr) {
    return;
  }
  portENTER_CRITICAL(&s_holder_mux);
  if (s_holder == xTaskGetCurrentTaskHandle()) {
    s_holder = nullptr;
  }
  portEXIT_CRITICAL(&s_holder_mux);
  xSemaphoreGive(s_sem);
}

bool heldBy(TaskHandle_t task) {
  if (task == nullptr) {
    return false;
  }
  portENTER_CRITICAL(&s_holder_mux);
  const bool held = (s_holder == task);
  portEXIT_CRITICAL(&s_holder_mux);
  return held;
}

void forceUnlockIfHeldBy(TaskHandle_t task) {
  (void)reclaimAfterTaskDeleted(task);
}

bool reclaimAfterTaskDeleted(TaskHandle_t deleted) {
  ensureSem();
  if (s_sem == nullptr || deleted == nullptr) {
    return false;
  }

  portENTER_CRITICAL(&s_holder_mux);
  const TaskHandle_t holder = s_holder;
  const UBaseType_t count = uxSemaphoreGetCount(s_sem);
  portEXIT_CRITICAL(&s_holder_mux);

  if (count > 0) {
    // Already free — drop a stale pointer to the deleted task if present.
    portENTER_CRITICAL(&s_holder_mux);
    if (s_holder == deleted) {
      s_holder = nullptr;
    }
    portEXIT_CRITICAL(&s_holder_mux);
    return false;
  }

  // Semaphore is taken. Reclaim only if the deleted task owned it, or if the
  // owner was never recorded (gap between Take and s_holder assign).
  if (holder != deleted && holder != nullptr) {
    return false;
  }

  portENTER_CRITICAL(&s_holder_mux);
  s_holder = nullptr;
  portEXIT_CRITICAL(&s_holder_mux);
  xSemaphoreGive(s_sem);
  return true;
}

void forceUnlock() {
  ensureSem();
  if (s_sem == nullptr) {
    return;
  }
  portENTER_CRITICAL(&s_holder_mux);
  s_holder = nullptr;
  portEXIT_CRITICAL(&s_holder_mux);
  xSemaphoreGive(s_sem);
}

bool busy() {
  ensureSem();
  if (s_sem == nullptr) {
    return false;
  }
  // Prefer count probe — Take/Give can race with a real holder on dual-core.
  return uxSemaphoreGetCount(s_sem) == 0;
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
