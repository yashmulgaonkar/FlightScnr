#include "services/clock_format.h"

namespace services::clock {

void formatCivilDate(const struct tm& local, bool numeric, char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  // On a too-small buffer strftime returns 0 and leaves the contents
  // indeterminate, so re-terminate explicitly to honour the header contract.
  if (strftime(out, len, numeric ? "%d.%m.%Y" : "%a, %b %d", &local) == 0) {
    out[0] = '\0';
  }
}

}  // namespace services::clock
