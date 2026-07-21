#include <unity.h>

#include <cstring>

#include "services/watch_type_parse.h"

using services::alert::isValidWatchType;
using services::alert::kWatchTypeLen;
using services::alert::kWatchTypeMax;
using services::alert::normalizeWatchType;
using services::alert::parseWatchTypeBlob;
using services::alert::rebuildWatchTypeBlob;
using services::alert::watchTypeListContains;

void setUp(void) {}
void tearDown(void) {}

void test_normalize_basic(void) {
  char out[kWatchTypeLen];
  TEST_ASSERT_TRUE(normalizeWatchType("b738", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("B738", out);
  TEST_ASSERT_TRUE(normalizeWatchType(" A333 ", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("A333", out);
  TEST_ASSERT_TRUE(normalizeWatchType("e75l", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("E75L", out);
}

void test_reject_marketing_and_short(void) {
  char out[kWatchTypeLen];
  TEST_ASSERT_FALSE(normalizeWatchType("A330-743", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchType("A330743", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchType("A", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchType("", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchType(nullptr, out, sizeof(out)));
}

void test_is_valid(void) {
  TEST_ASSERT_TRUE(isValidWatchType("B738"));
  TEST_ASSERT_TRUE(isValidWatchType("C2"));
  TEST_ASSERT_FALSE(isValidWatchType("A"));
  TEST_ASSERT_FALSE(isValidWatchType("B73-"));
}

void test_parse_blob_dedupe_and_skip_invalid(void) {
  char dest[kWatchTypeMax][kWatchTypeLen];
  const size_t n =
      parseWatchTypeBlob("B738, a333, A330-743, B738, , E75L", dest, kWatchTypeMax);
  TEST_ASSERT_EQUAL_UINT(3, n);
  TEST_ASSERT_EQUAL_STRING("B738", dest[0]);
  TEST_ASSERT_EQUAL_STRING("A333", dest[1]);
  TEST_ASSERT_EQUAL_STRING("E75L", dest[2]);
}

void test_rebuild_and_contains(void) {
  char dest[kWatchTypeMax][kWatchTypeLen];
  const size_t n = parseWatchTypeBlob("B738,A333", dest, kWatchTypeMax);
  char blob[96];
  rebuildWatchTypeBlob(dest, n, blob, sizeof(blob));
  TEST_ASSERT_EQUAL_STRING("B738,A333", blob);
  TEST_ASSERT_TRUE(watchTypeListContains(dest, n, "b738"));
  TEST_ASSERT_TRUE(watchTypeListContains(dest, n, "A333"));
  TEST_ASSERT_FALSE(watchTypeListContains(dest, n, "A320"));
  TEST_ASSERT_FALSE(watchTypeListContains(dest, 0, "B738"));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_normalize_basic);
  RUN_TEST(test_reject_marketing_and_short);
  RUN_TEST(test_is_valid);
  RUN_TEST(test_parse_blob_dedupe_and_skip_invalid);
  RUN_TEST(test_rebuild_and_contains);
  return UNITY_END();
}
