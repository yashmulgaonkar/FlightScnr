#pragma once

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace services::https {

/** Create the global TLS lock (safe to call more than once). */
void init();

/** Wait up to timeout_ms for exclusive HTTPS access. Returns false if timed out. */
bool lock(uint32_t timeout_ms);

void unlock();

/** True when another task holds the global HTTPS lock. */
bool busy();

/** True when task currently owns the HTTPS lock. */
bool heldBy(TaskHandle_t task);

/**
 * Release the HTTPS lock only if held by task (e.g. after vTaskDelete of that
 * holder). No-op if a different live task owns the lock — never steal from
 * ADS-B/photo mid-request.
 */
void forceUnlockIfHeldBy(TaskHandle_t task);

/**
 * After vTaskDelete(deleted): free the lock if that task owned it, or if the
 * semaphore is taken with no recorded owner (Take/holder TOCTOU orphan).
 * Never steals from a different live holder. Returns true if a Give ran.
 */
bool reclaimAfterTaskDeleted(TaskHandle_t deleted);

/**
 * Unconditionally mark the lock available. Prefer reclaimAfterTaskDeleted after
 * killing a known holder. Safe no-op if already free (binary semaphore).
 */
void forceUnlock();

/** RAII guard — releases the lock on destruction if acquire succeeded. */
class ScopedLock {
 public:
  explicit ScopedLock(uint32_t timeout_ms = 15000);
  ~ScopedLock();

  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;

  bool held() const { return held_; }

 private:
  bool held_ = false;
};

}  // namespace services::https
