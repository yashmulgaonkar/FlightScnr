#include <unity.h>

#include "services/altitude_filter.h"

using services::adsb::altitudeWithinBand;

void setUp(void) {}
void tearDown(void) {}

// (a) both limits off -> always true, regardless of has_altitude / alt.
void test_both_off_always_true(void) {
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 1000.0f, 0, 0));
  TEST_ASSERT_TRUE(altitudeWithinBand(false, 0.0f, 0, 0));
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 42000.0f, -5, -3));
}

// (b) floor only: below is out, at boundary is in (inclusive), above is in.
void test_floor_only(void) {
  TEST_ASSERT_FALSE(altitudeWithinBand(true, 400.0f, 500, 0));
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 500.0f, 500, 0));   // inclusive
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 600.0f, 500, 0));
}

// (c) ceiling only: above is out, at boundary is in (inclusive), below is in.
void test_ceiling_only(void) {
  TEST_ASSERT_FALSE(altitudeWithinBand(true, 12000.0f, 0, 10000));
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 10000.0f, 0, 10000));  // inclusive
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 8000.0f, 0, 10000));
}

// (d) band floor..ceiling: inside true, below floor false, above ceiling false.
void test_band(void) {
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 5000.0f, 500, 10000));
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 500.0f, 500, 10000));    // low boundary
  TEST_ASSERT_TRUE(altitudeWithinBand(true, 10000.0f, 500, 10000));  // high boundary
  TEST_ASSERT_FALSE(altitudeWithinBand(true, 400.0f, 500, 10000));
  TEST_ASSERT_FALSE(altitudeWithinBand(true, 10001.0f, 500, 10000));
}

// (e) any limit active + has_altitude=false -> false.
void test_unknown_altitude_excluded_when_active(void) {
  TEST_ASSERT_FALSE(altitudeWithinBand(false, 0.0f, 500, 0));
  TEST_ASSERT_FALSE(altitudeWithinBand(false, 0.0f, 0, 10000));
  TEST_ASSERT_FALSE(altitudeWithinBand(false, 0.0f, 500, 10000));
}

// (f) floor > ceiling -> empty band -> nothing passes.
void test_floor_above_ceiling_empty_band(void) {
  TEST_ASSERT_FALSE(altitudeWithinBand(true, 5000.0f, 10000, 500));
  TEST_ASSERT_FALSE(altitudeWithinBand(true, 300.0f, 10000, 500));
  TEST_ASSERT_FALSE(altitudeWithinBand(true, 20000.0f, 10000, 500));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_both_off_always_true);
  RUN_TEST(test_floor_only);
  RUN_TEST(test_ceiling_only);
  RUN_TEST(test_band);
  RUN_TEST(test_unknown_altitude_excluded_when_active);
  RUN_TEST(test_floor_above_ceiling_empty_band);
  return UNITY_END();
}
