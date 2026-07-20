#pragma once

#include <ArduinoJson.h>

#include <cstddef>

#include "services/weather.h"

namespace services::weather {

/**
 * Map a WMO weather-interpretation code (Open-Meteo `weather_code`) onto the
 * Tomorrow.io base weather codes the firmware already renders icons for. Every
 * target has an icon; unknown WMO codes fall back to 1001 (cloudy) so the icon
 * layer always has something safe to draw.
 */
int wmoToWeatherCode(int wmo);

/**
 * Configure a deserialization filter limiting the parsed document to the
 * `current.*` and `daily.*` fields consumed by parseOpenMeteo (heap guard,
 * analogous to the adsbdb / Tomorrow.io filters).
 */
void buildOpenMeteoFilter(ArduinoJson::JsonDocument& filter);

/**
 * Parse an Open-Meteo `GET /v1/forecast` response (requested with
 * `timeformat=unixtime`, so all times are Unix-epoch UTC integers) into a
 * WeatherData. Pure, hardware-free transformation (test seam).
 *
 * Fills current conditions (temperature/humidity/WMO code → Tomorrow.io code),
 * sunrise/sunset from `daily[0]`, and up to kForecastDays daily buckets
 * (date, mapped code, min/max temperature, precipitation probability → -1 when
 * absent). Temperature is taken as-is in the requested unit; the caller sets
 * `out->imperial`. On success sets `out->source = Source::OpenMeteo` and
 * `out->valid = true`.
 *
 * Returns false for an empty/broken response or when the current block has no
 * usable data.
 */
bool parseOpenMeteo(ArduinoJson::JsonDocument& doc, WeatherData* out);

}  // namespace services::weather
