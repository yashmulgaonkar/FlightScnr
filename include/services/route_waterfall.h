#pragma once

#include "services/route_info.h"

namespace services::route {

/**
 * True when both origin and dest are non-empty.
 * Pure, hardware-free (host-testable seam).
 */
bool routeEndpointsComplete(const RouteInfo& r);

/**
 * Whether a live paid-API step should terminate the detail waterfall.
 * Requires a complete origin/dest pair so airline-only hits do not block
 * later providers (FA / FR24 / adsbdb) from filling the route.
 */
bool shouldFinishLiveApiStep(const RouteInfo& r);

/**
 * Copy non-empty fields from `partial` into `dest` without overwriting
 * fields that are already set. Used when an earlier provider returns
 * airline-only data and the waterfall continues.
 */
void mergePartialRoute(RouteInfo* dest, const RouteInfo& partial);

}  // namespace services::route
