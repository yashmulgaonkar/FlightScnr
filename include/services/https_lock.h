#pragma once

#include <cstdint>

namespace services::https {

/** Create the global TLS mutex (safe to call more than once). */
void init();

/** Wait up to timeout_ms for exclusive HTTPS access. Returns false if timed out. */
bool lock(uint32_t timeout_ms);

void unlock();

/** True when another task holds the global HTTPS mutex. */
bool busy();

/**
 * Release the HTTPS mutex after a holder was killed (e.g. route worker
 * vTaskDelete mid-request). Safe no-op if the mutex is already free.
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
