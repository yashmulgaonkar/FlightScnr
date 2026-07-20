#include <ArduinoJson.h>
#include <unity.h>

#include "services/adsbdb_parse.h"
#include "services/route_info.h"

using services::route::buildAdsbdbFilter;
using services::route::parseAdsbdbResponse;
using services::route::RouteInfo;

namespace {

// Parse a JSON string through the adsbdb deserialization filter, exactly as the
// runtime does, then run the pure parser over the result.
bool parseJson(const char* json, RouteInfo* route) {
  JsonDocument filter;
  buildAdsbdbFilter(filter);
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, json, DeserializationOption::Filter(filter));
  TEST_ASSERT_EQUAL_MESSAGE(DeserializationError::Ok, err.code(), err.c_str());
  return parseAdsbdbResponse(doc, route);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// (a) full route + airline
void test_full_route_with_airline(void) {
  const char* json =
      "{\"response\":{\"flightroute\":{"
      "\"callsign\":\"BAW123\","
      "\"airline\":{\"name\":\"British Airways\",\"icao\":\"BAW\",\"iata\":\"BA\"},"
      "\"origin\":{\"icao_code\":\"EGLL\",\"iata_code\":\"LHR\"},"
      "\"destination\":{\"icao_code\":\"KJFK\",\"iata_code\":\"JFK\"}"
      "}}}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("EGLL", r.origin);
  TEST_ASSERT_EQUAL_STRING("KJFK", r.dest);
  TEST_ASSERT_EQUAL_STRING("British Airways", r.airline);
  TEST_ASSERT_EQUAL_STRING("BAW", r.airline_icao);
}

// (b) route without airline (airline: null) -> origin/dest set, airline empty, true
void test_route_without_airline(void) {
  const char* json =
      "{\"response\":{\"flightroute\":{"
      "\"airline\":null,"
      "\"origin\":{\"icao_code\":\"EDDF\"},"
      "\"destination\":{\"icao_code\":\"LEMD\"}"
      "}}}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("EDDF", r.origin);
  TEST_ASSERT_EQUAL_STRING("LEMD", r.dest);
  TEST_ASSERT_EQUAL_STRING("", r.airline);
  TEST_ASSERT_EQUAL_STRING("", r.airline_icao);
}

// (c) airline without a usable route
void test_airline_without_route(void) {
  const char* json =
      "{\"response\":{\"flightroute\":{"
      "\"airline\":{\"name\":\"Lufthansa\",\"icao\":\"DLH\"}"
      "}}}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("", r.origin);
  TEST_ASSERT_EQUAL_STRING("", r.dest);
  TEST_ASSERT_EQUAL_STRING("Lufthansa", r.airline);
  TEST_ASSERT_EQUAL_STRING("DLH", r.airline_icao);
}

// (d) unknown callsign -> response is a string, not an object -> false
void test_unknown_callsign(void) {
  const char* json = "{\"response\":\"unknown callsign\"}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_STRING("", r.origin);
  TEST_ASSERT_EQUAL_STRING("", r.dest);
  TEST_ASSERT_EQUAL_STRING("", r.airline);
  TEST_ASSERT_EQUAL_STRING("", r.airline_icao);
}

// (e) missing / partial fields
void test_partial_fields(void) {
  // Only origin present; airline icao is not 3 chars -> dropped.
  const char* json =
      "{\"response\":{\"flightroute\":{"
      "\"airline\":{\"name\":\"\",\"icao\":\"XX\"},"
      "\"origin\":{\"icao_code\":\"KLAX\"}"
      "}}}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("KLAX", r.origin);
  TEST_ASSERT_EQUAL_STRING("", r.dest);
  TEST_ASSERT_EQUAL_STRING("", r.airline);
  TEST_ASSERT_EQUAL_STRING("", r.airline_icao);
}

// empty flightroute object -> no usable data -> false
void test_empty_flightroute(void) {
  const char* json = "{\"response\":{\"flightroute\":{}}}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_STRING("", r.origin);
  TEST_ASSERT_EQUAL_STRING("", r.dest);
}

// (f) unexpected / extra fields are ignored by the filter + parser
void test_extra_fields_ignored(void) {
  const char* json =
      "{\"response\":{\"flightroute\":{"
      "\"callsign\":\"AAL55\","
      "\"callsign_icao\":\"AAL55\","
      "\"airline\":{\"name\":\"American Airlines\",\"icao\":\"AAL\","
      "\"country\":\"United States\",\"country_iso\":\"US\"},"
      "\"origin\":{\"icao_code\":\"KDFW\",\"latitude\":32.9,\"municipality\":\"Dallas\"},"
      "\"destination\":{\"icao_code\":\"KORD\",\"elevation\":668}"
      "},\"extra_top\":123},\"unexpected\":true}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("KDFW", r.origin);
  TEST_ASSERT_EQUAL_STRING("KORD", r.dest);
  TEST_ASSERT_EQUAL_STRING("American Airlines", r.airline);
  TEST_ASSERT_EQUAL_STRING("AAL", r.airline_icao);
}

// icao_code with lowercase / punctuation -> uppercased, alnum-only, max 4
void test_icao_normalisation(void) {
  const char* json =
      "{\"response\":{\"flightroute\":{"
      "\"origin\":{\"icao_code\":\"eg-ll\"},"
      "\"destination\":{\"icao_code\":\"kjfkx\"}"
      "}}}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("EGLL", r.origin);
  TEST_ASSERT_EQUAL_STRING("KJFK", r.dest);
}

// long airline name is truncated to the RouteInfo buffer (27 chars + NUL)
void test_airline_name_truncated(void) {
  const char* json =
      "{\"response\":{\"flightroute\":{"
      "\"airline\":{\"name\":\"This Airline Name Is Way Too Long To Fit\",\"icao\":\"ABC\"}"
      "}}}";
  RouteInfo r;
  const bool ok = parseJson(json, &r);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT(27, strlen(r.airline));
  TEST_ASSERT_EQUAL_STRING("ABC", r.airline_icao);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_full_route_with_airline);
  RUN_TEST(test_route_without_airline);
  RUN_TEST(test_airline_without_route);
  RUN_TEST(test_unknown_callsign);
  RUN_TEST(test_partial_fields);
  RUN_TEST(test_empty_flightroute);
  RUN_TEST(test_extra_fields_ignored);
  RUN_TEST(test_icao_normalisation);
  RUN_TEST(test_airline_name_truncated);
  return UNITY_END();
}
