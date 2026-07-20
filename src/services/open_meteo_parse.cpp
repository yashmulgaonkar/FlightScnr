#include "services/open_meteo_parse.h"

#include <cstddef>

namespace services::weather {

namespace {

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObject;
using ArduinoJson::JsonObjectConst;

}  // namespace

int wmoToWeatherCode(int wmo) {
  // WMO weather-interpretation codes -> Tomorrow.io base codes (all have icons).
  switch (wmo) {
    case 0:            return 1000;  // clear sky
    case 1:            return 1100;  // mainly clear
    case 2:            return 1101;  // partly cloudy
    case 3:            return 1001;  // overcast
    case 45:
    case 48:           return 2000;  // fog
    case 51:
    case 53:
    case 55:           return 4000;  // drizzle
    case 56:
    case 57:           return 6000;  // freezing drizzle
    case 61:           return 4200;  // slight rain
    case 63:           return 4001;  // moderate rain
    case 65:           return 4201;  // heavy rain
    case 66:
    case 67:           return 6001;  // freezing rain
    case 71:           return 5100;  // slight snow
    case 73:           return 5000;  // moderate snow
    case 75:           return 5101;  // heavy snow
    case 77:           return 5000;  // snow grains
    case 80:           return 4200;  // slight rain showers
    case 81:           return 4001;  // moderate rain showers
    case 82:           return 4201;  // violent rain showers
    case 85:           return 5100;  // slight snow showers
    case 86:           return 5101;  // heavy snow showers
    case 95:           return 8000;  // thunderstorm
    case 96:
    case 99:           return 8000;  // thunderstorm with hail
    default:           return 1001;  // unknown -> cloudy fallback
  }
}

void buildOpenMeteoFilter(JsonDocument& filter) {
  JsonObject current = filter["current"].to<JsonObject>();
  current["temperature_2m"] = true;
  current["relative_humidity_2m"] = true;
  current["weather_code"] = true;

  JsonObject daily = filter["daily"].to<JsonObject>();
  daily["time"] = true;
  daily["weather_code"] = true;
  daily["temperature_2m_max"] = true;
  daily["temperature_2m_min"] = true;
  daily["precipitation_probability_max"] = true;
  daily["sunrise"] = true;
  daily["sunset"] = true;
}

bool parseOpenMeteo(JsonDocument& doc, WeatherData* out) {
  if (out == nullptr) {
    return false;
  }

  JsonObjectConst current = doc["current"].as<JsonObjectConst>();
  if (current.isNull()) {
    return false;
  }
  // Require at least a usable current temperature reading — an empty/broken
  // response has no numeric temperature_2m.
  if (!current["temperature_2m"].is<float>()) {
    return false;
  }

  out->current_temp = current["temperature_2m"].as<float>();
  out->current_humidity = current["relative_humidity_2m"].as<int>();
  out->current_code = wmoToWeatherCode(current["weather_code"].as<int>());

  JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
  if (!daily.isNull()) {
    JsonArrayConst time = daily["time"].as<JsonArrayConst>();
    JsonArrayConst code = daily["weather_code"].as<JsonArrayConst>();
    JsonArrayConst tmax = daily["temperature_2m_max"].as<JsonArrayConst>();
    JsonArrayConst tmin = daily["temperature_2m_min"].as<JsonArrayConst>();
    JsonArrayConst precip = daily["precipitation_probability_max"].as<JsonArrayConst>();
    JsonArrayConst sunrise = daily["sunrise"].as<JsonArrayConst>();
    JsonArrayConst sunset = daily["sunset"].as<JsonArrayConst>();

    if (!sunrise.isNull() && sunrise.size() > 0) {
      out->sunrise_epoch = sunrise[0].as<int64_t>();
    }
    if (!sunset.isNull() && sunset.size() > 0) {
      out->sunset_epoch = sunset[0].as<int64_t>();
    }

    if (!time.isNull()) {
      const size_t n = time.size();
      for (int i = 0; i < kForecastDays && static_cast<size_t>(i) < n; ++i) {
        DayForecast& fc = out->days[i];
        fc.date_epoch = time[i].as<int64_t>();
        fc.weather_code =
            wmoToWeatherCode(!code.isNull() && static_cast<size_t>(i) < code.size()
                                 ? code[i].as<int>()
                                 : 1001);
        if (!tmax.isNull() && static_cast<size_t>(i) < tmax.size()) {
          fc.temp_max = tmax[i].as<float>();
        }
        if (!tmin.isNull() && static_cast<size_t>(i) < tmin.size()) {
          fc.temp_min = tmin[i].as<float>();
        }
        fc.precip_probability =
            (!precip.isNull() && static_cast<size_t>(i) < precip.size() &&
             precip[i].is<int>())
                ? precip[i].as<int>()
                : -1;
        fc.valid = true;
      }
    }
  }

  out->source = WeatherData::Source::OpenMeteo;
  out->valid = true;
  return true;
}

}  // namespace services::weather
