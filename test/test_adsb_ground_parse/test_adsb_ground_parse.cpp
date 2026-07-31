#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

// Mirrors services::adsb filter + ground detection against a real adsb.fi snippet.

static void buildAircraftFilter(JsonDocument& filter) {
  JsonObject el = filter["ac"].add<JsonObject>();
  static const char* kKeepKeys[] = {
      "lat",          "lon",      "true_heading", "mag_heading", "track",
      "dir",          "gs",       "tas",          "ias",         "baro_rate",
      "geom_rate",    "alt_baro", "alt_geom",     "flight",      "hex",
      "t",            "r",        "dbFlags",      "category",    "squawk"};
  for (const char* key : kKeepKeys) {
    el[key] = true;
  }
}

static bool isOnGround(JsonObjectConst plane) {
  const char* s = plane["alt_baro"].as<const char*>();
  return s != nullptr && strcmp(s, "ground") == 0;
}

static bool readJsonFloat(JsonObjectConst obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

// Minimal LAX-like payload: one airborne, one taxiing airliner, two C2 trucks.
static const char kSample[] = R"({
  "ac": [
    {"hex":"aaa111","lat":33.95,"lon":-118.40,"alt_baro":3500,"category":"A3","gs":220,"flight":"AIR1"},
    {"hex":"bbb222","lat":33.94,"lon":-118.41,"alt_baro":"ground","category":"A3","gs":18,"flight":"TAXI1","t":"B738"},
    {"hex":"ccc333","lat":33.937,"lon":-118.423,"alt_baro":"ground","category":"C2","gs":12,"flight":"B08"},
    {"hex":"ddd444","lat":33.944,"lon":-118.412,"alt_baro":"ground","category":"C2","gs":8,"flight":"B653"}
  ]
})";

void test_filter_keeps_ground_string_alt(void) {
  JsonDocument filter;
  buildAircraftFilter(filter);
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, kSample, DeserializationOption::Filter(filter));
  TEST_ASSERT_FALSE(err);

  JsonArray ac = doc["ac"].as<JsonArray>();
  TEST_ASSERT_EQUAL(4, ac.size());

  int ground = 0;
  int trucks = 0;
  for (JsonObjectConst plane : ac) {
    float lat = 0, lon = 0;
    TEST_ASSERT_TRUE(readJsonFloat(plane, "lat", &lat));
    TEST_ASSERT_TRUE(readJsonFloat(plane, "lon", &lon));
    if (isOnGround(plane)) {
      ++ground;
      const char* cat = plane["category"].as<const char*>();
      if (cat != nullptr && cat[0] == 'C') {
        ++trucks;
      }
      // String "ground" must not look numeric.
      TEST_ASSERT_FALSE(plane["alt_baro"].is<float>());
      TEST_ASSERT_FALSE(plane["alt_baro"].is<int>());
    }
  }
  TEST_ASSERT_EQUAL(3, ground);
  TEST_ASSERT_EQUAL(2, trucks);
}

void test_is_float_rejects_ground_string(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"alt_baro\":\"ground\"}");
  JsonObjectConst plane = doc.as<JsonObjectConst>();
  TEST_ASSERT_TRUE(isOnGround(plane));
  TEST_ASSERT_FALSE(plane["alt_baro"].is<float>());
  const char* s = plane["alt_baro"].as<const char*>();
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_STRING("ground", s);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_is_float_rejects_ground_string);
  RUN_TEST(test_filter_keeps_ground_string_alt);
  return UNITY_END();
}
