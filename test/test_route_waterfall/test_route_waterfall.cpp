#include <cstring>
#include <unity.h>

#include "services/route_info.h"
#include "services/route_waterfall.h"

using services::route::mergePartialRoute;
using services::route::routeEndpointsComplete;
using services::route::RouteInfo;
using services::route::shouldFinishLiveApiStep;

namespace {

void clearRoute(RouteInfo* r) {
  r->airline[0] = '\0';
  r->airline_icao[0] = '\0';
  r->origin[0] = '\0';
  r->dest[0] = '\0';
}

void setField(char* out, size_t out_len, const char* value) {
  std::strncpy(out, value, out_len - 1);
  out[out_len - 1] = '\0';
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_endpoints_complete_requires_both(void) {
  RouteInfo r;
  clearRoute(&r);
  TEST_ASSERT_FALSE(routeEndpointsComplete(r));
  TEST_ASSERT_FALSE(shouldFinishLiveApiStep(r));

  setField(r.origin, sizeof(r.origin), "KJFK");
  TEST_ASSERT_FALSE(routeEndpointsComplete(r));
  TEST_ASSERT_FALSE(shouldFinishLiveApiStep(r));

  setField(r.dest, sizeof(r.dest), "EGLL");
  TEST_ASSERT_TRUE(routeEndpointsComplete(r));
  TEST_ASSERT_TRUE(shouldFinishLiveApiStep(r));
}

void test_airline_only_does_not_finish_waterfall(void) {
  RouteInfo r;
  clearRoute(&r);
  setField(r.airline, sizeof(r.airline), "British Airways");
  setField(r.airline_icao, sizeof(r.airline_icao), "BAW");
  TEST_ASSERT_FALSE(shouldFinishLiveApiStep(r));
}

void test_merge_partial_fills_empty_only(void) {
  RouteInfo dest;
  clearRoute(&dest);
  setField(dest.airline, sizeof(dest.airline), "Keep Me");

  RouteInfo partial;
  clearRoute(&partial);
  setField(partial.airline, sizeof(partial.airline), "Overwrite?");
  setField(partial.airline_icao, sizeof(partial.airline_icao), "AAL");
  setField(partial.origin, sizeof(partial.origin), "KDFW");
  setField(partial.dest, sizeof(partial.dest), "KLAX");

  mergePartialRoute(&dest, partial);
  TEST_ASSERT_EQUAL_STRING("Keep Me", dest.airline);
  TEST_ASSERT_EQUAL_STRING("AAL", dest.airline_icao);
  TEST_ASSERT_EQUAL_STRING("KDFW", dest.origin);
  TEST_ASSERT_EQUAL_STRING("KLAX", dest.dest);
}

void test_merge_partial_null_dest_is_safe(void) {
  RouteInfo partial;
  clearRoute(&partial);
  setField(partial.origin, sizeof(partial.origin), "KJFK");
  mergePartialRoute(nullptr, partial);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_endpoints_complete_requires_both);
  RUN_TEST(test_airline_only_does_not_finish_waterfall);
  RUN_TEST(test_merge_partial_fills_empty_only);
  RUN_TEST(test_merge_partial_null_dest_is_safe);
  return UNITY_END();
}
