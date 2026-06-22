#include "services/https_lock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace services::https {

namespace {

SemaphoreHandle_t s_mutex = nullptr;

void ensureMutex() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

}  // namespace

void init() { ensureMutex(); }

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
