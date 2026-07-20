#include "services/clock_format.h"

#include <cstring>

namespace services::clock {

void formatCivilDate(const struct tm& local, bool numeric, char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  out[0] = '\0';
  strftime(out, len, numeric ? "%d.%m.%Y" : "%a, %b %d", &local);
}

}  // namespace services::clock
