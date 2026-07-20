#include <ArduinoJson.h>
#include <unity.h>

#include "services/open_meteo_parse.h"
#include "services/weather.h"

using services::weather::buildOpenMeteoFilter;
using services::weather::kForecastDays;
using services::weather::parseOpenMeteo;
using services::weather::WeatherData;
using services::weather::wmoToWeatherCode;

namespace {

// Parse a JSON string through the Open-Meteo deserialization filter, exactly as
// the runtime does, then run the pure parser over the result.
bool parseJson(const char* json, WeatherData* out) {
  JsonDocument filter;
  buildOpenMeteoFilter(filter);
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, json, DeserializationOption::Filter(filter));
  TEST_ASSERT_EQUAL_MESSAGE(DeserializationError::Ok, err.code(), err.c_str());
  return parseOpenMeteo(doc, out);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// (a) full response: current + 3 daily days, unixtime epochs.
void test_full_response(void) {
  const char* json =
      "{\"latitude\":52.52,\"longitude\":13.41,"
      "\"current\":{\"time\":1721460000,\"temperature_2m\":21.4,"
      "\"relative_humidity_2m\":63,\"weather_code\":3},"
      "\"daily\":{"
      "\"time\":[1721433600,1721520000,1721606400],"
      "\"weather_code\":[3,61,95],"
      "\"temperature_2m_max\":[24.5,19.0,26.1],"
      "\"temperature_2m_min\":[14.2,12.8,16.0],"
      "\"precipitation_probability_max\":[10,80,40],"
      "\"sunrise\":[1721446800,1721533260,1721619720],"
      "\"sunset\":[1721503200,1721589540,1721675880]}}";
  WeatherData wx;
  const bool ok = parseJson(json, &wx);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(wx.valid);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WeatherData::Source::OpenMeteo),
                        static_cast<int>(wx.source));
  // The parser must not touch the unit flag — the caller owns it.
  TEST_ASSERT_FALSE(wx.imperial);

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.4f, wx.current_temp);
  TEST_ASSERT_EQUAL_INT(63, wx.current_humidity);
  TEST_ASSERT_EQUAL_INT(1001, wx.current_code);  // WMO 3 -> cloudy

  TEST_ASSERT_EQUAL_INT64(1721446800, wx.sunrise_epoch);
  TEST_ASSERT_EQUAL_INT64(1721503200, wx.sunset_epoch);

  TEST_ASSERT_TRUE(wx.days[0].valid);
  TEST_ASSERT_EQUAL_INT64(1721433600, wx.days[0].date_epoch);
  TEST_ASSERT_EQUAL_INT(1001, wx.days[0].weather_code);  // WMO 3
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 24.5f, wx.days[0].temp_max);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 14.2f, wx.days[0].temp_min);
  TEST_ASSERT_EQUAL_INT(10, wx.days[0].precip_probability);

  TEST_ASSERT_TRUE(wx.days[1].valid);
  TEST_ASSERT_EQUAL_INT64(1721520000, wx.days[1].date_epoch);
  TEST_ASSERT_EQUAL_INT(4200, wx.days[1].weather_code);  // WMO 61 -> light rain
  TEST_ASSERT_EQUAL_INT(80, wx.days[1].precip_probability);

  TEST_ASSERT_TRUE(wx.days[2].valid);
  TEST_ASSERT_EQUAL_INT64(1721606400, wx.days[2].date_epoch);
  TEST_ASSERT_EQUAL_INT(8000, wx.days[2].weather_code);  // WMO 95 -> thunderstorm
  TEST_ASSERT_EQUAL_INT(40, wx.days[2].precip_probability);
}

// (b) WMO mapping spot checks.
void test_wmo_mapping(void) {
  TEST_ASSERT_EQUAL_INT(1000, wmoToWeatherCode(0));   // clear
  TEST_ASSERT_EQUAL_INT(1100, wmoToWeatherCode(1));   // mostly clear
  TEST_ASSERT_EQUAL_INT(1101, wmoToWeatherCode(2));   // partly cloudy
  TEST_ASSERT_EQUAL_INT(1001, wmoToWeatherCode(3));   // overcast
  TEST_ASSERT_EQUAL_INT(2000, wmoToWeatherCode(45));  // fog
  TEST_ASSERT_EQUAL_INT(4000, wmoToWeatherCode(51));  // drizzle
  TEST_ASSERT_EQUAL_INT(4200, wmoToWeatherCode(61));  // light rain
  TEST_ASSERT_EQUAL_INT(4001, wmoToWeatherCode(63));  // rain
  TEST_ASSERT_EQUAL_INT(4201, wmoToWeatherCode(65));  // heavy rain
  TEST_ASSERT_EQUAL_INT(5100, wmoToWeatherCode(71));  // light snow
  TEST_ASSERT_EQUAL_INT(5000, wmoToWeatherCode(73));  // snow
  TEST_ASSERT_EQUAL_INT(8000, wmoToWeatherCode(95));  // thunderstorm
  TEST_ASSERT_EQUAL_INT(8000, wmoToWeatherCode(96));  // thunderstorm + hail
  TEST_ASSERT_EQUAL_INT(1001, wmoToWeatherCode(1234)); // unknown -> cloudy fallback
}

// (c) missing precipitation_probability_max -> -1.
void test_missing_precip(void) {
  const char* json =
      "{\"current\":{\"temperature_2m\":10.0,\"relative_humidity_2m\":80,"
      "\"weather_code\":0},"
      "\"daily\":{"
      "\"time\":[1721433600],"
      "\"weather_code\":[0],"
      "\"temperature_2m_max\":[12.0],"
      "\"temperature_2m_min\":[5.0],"
      "\"sunrise\":[1721446800],"
      "\"sunset\":[1721503200]}}";
  WeatherData wx;
  const bool ok = parseJson(json, &wx);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(wx.days[0].valid);
  TEST_ASSERT_EQUAL_INT(-1, wx.days[0].precip_probability);
  TEST_ASSERT_FALSE(wx.days[1].valid);
  TEST_ASSERT_FALSE(wx.days[2].valid);
}

// (d) empty / broken JSON -> false.
void test_empty_response(void) {
  WeatherData wx;
  TEST_ASSERT_FALSE(parseJson("{}", &wx));
  TEST_ASSERT_FALSE(wx.valid);

  WeatherData wx2;
  // current with no usable fields.
  TEST_ASSERT_FALSE(parseJson("{\"daily\":{\"time\":[1721433600]}}", &wx2));
}

// (e) extra / unexpected fields are dropped by the filter and ignored.
void test_extra_fields_ignored(void) {
  const char* json =
      "{\"generationtime_ms\":0.12,\"utc_offset_seconds\":0,"
      "\"timezone\":\"GMT\",\"elevation\":38.0,"
      "\"current_units\":{\"temperature_2m\":\"°C\"},"
      "\"current\":{\"time\":1721460000,\"interval\":900,"
      "\"temperature_2m\":18.5,\"relative_humidity_2m\":55,\"weather_code\":1,"
      "\"apparent_temperature\":17.9},"
      "\"daily_units\":{\"time\":\"unixtime\"},"
      "\"daily\":{"
      "\"time\":[1721433600],"
      "\"weather_code\":[1],"
      "\"temperature_2m_max\":[20.0],"
      "\"temperature_2m_min\":[9.0],"
      "\"precipitation_probability_max\":[5],"
      "\"uv_index_max\":[6.5],"
      "\"sunrise\":[1721446800],"
      "\"sunset\":[1721503200]}}";
  WeatherData wx;
  const bool ok = parseJson(json, &wx);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.5f, wx.current_temp);
  TEST_ASSERT_EQUAL_INT(55, wx.current_humidity);
  TEST_ASSERT_EQUAL_INT(1100, wx.current_code);  // WMO 1
  TEST_ASSERT_EQUAL_INT(5, wx.days[0].precip_probability);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_full_response);
  RUN_TEST(test_wmo_mapping);
  RUN_TEST(test_missing_precip);
  RUN_TEST(test_empty_response);
  RUN_TEST(test_extra_fields_ignored);
  return UNITY_END();
}
