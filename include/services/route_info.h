#pragma once

namespace services::route {

/** Plain route/airline record shared across route sources and the parser seam. */
struct RouteInfo {
  char airline[28];
  char airline_icao[4];
  char origin[5];
  char dest[5];
};

}  // namespace services::route
