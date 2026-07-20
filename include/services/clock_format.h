#pragma once

#include <cstddef>
#include <ctime>

namespace services::clock {

/** Format a local civil date into `out`.
 *  numeric=false -> "%a, %b %d" (e.g. "Mon, Jul 20").
 *  numeric=true  -> "%d.%m.%Y"  (e.g. "20.07.2026").
 *  Hardware-free (host-testable): reads only the fields already present in
 *  `local` (note `%a` uses tm_wday directly). Null/length guards apply and
 *  `out[0]` is always terminated when len > 0. */
void formatCivilDate(const struct tm& local, bool numeric, char* out, size_t len);

}  // namespace services::clock
