#pragma once

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>

#include "config.h"

namespace hardware {

inline void gfxLog(const char* msg) {
  if (config::kGfxDebug) {
    Serial.println(msg);
    Serial.flush();
  }
}

inline void gfxLogf(const char* fmt, ...) {
  if (!config::kGfxDebug) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  char buf[128];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
  Serial.flush();
}

}  // namespace hardware
