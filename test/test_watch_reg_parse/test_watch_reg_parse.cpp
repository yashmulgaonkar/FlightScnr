#include <unity.h>

#include "services/watch_reg_parse.h"

using services::alert::isValidWatchReg;
using services::alert::kWatchRegLen;
using services::alert::kWatchRegMax;
using services::alert::normalizeWatchReg;
using services::alert::parseWatchRegBlob;
using services::alert::rebuildWatchRegBlob;
using services::alert::watchRegListContains;

void setUp(void) {}
void tearDown(void) {}

void test_normalize_us_and_hyphenated(void) {
  char out[kWatchRegLen];
  TEST_ASSERT_TRUE(normalizeWatchReg("n2136u", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("N2136U", out);
  TEST_ASSERT_TRUE(normalizeWatchReg(" cs-tpq ", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("CS-TPQ", out);
}

void test_reject_invalid(void) {
  char out[kWatchRegLen];
  TEST_ASSERT_FALSE(normalizeWatchReg("AB", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchReg("12345", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchReg("CS--TPQ", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchReg("-CSTPQ", out, sizeof(out)));
  TEST_ASSERT_FALSE(normalizeWatchReg("", out, sizeof(out)));
}

void test_parse_dedupe_hyphen_insensitive(void) {
  char dest[kWatchRegMax][kWatchRegLen];
  const size_t n =
      parseWatchRegBlob("N2136U, cs-tpq, CSTPQ, N2136U, !!, AB", dest, kWatchRegMax);
  TEST_ASSERT_EQUAL_UINT(2, n);
  TEST_ASSERT_EQUAL_STRING("N2136U", dest[0]);
  TEST_ASSERT_EQUAL_STRING("CS-TPQ", dest[1]);
}

void test_contains_hyphen_insensitive(void) {
  char dest[kWatchRegMax][kWatchRegLen];
  const size_t n = parseWatchRegBlob("CS-TPQ,N2136U", dest, kWatchRegMax);
  TEST_ASSERT_TRUE(watchRegListContains(dest, n, "cstpq"));
  TEST_ASSERT_TRUE(watchRegListContains(dest, n, "CS-TPQ"));
  TEST_ASSERT_TRUE(watchRegListContains(dest, n, "n2136u"));
  TEST_ASSERT_FALSE(watchRegListContains(dest, n, "G-ABCD"));
}

void test_rebuild_blob(void) {
  char dest[kWatchRegMax][kWatchRegLen];
  const size_t n = parseWatchRegBlob("N2136U,CS-TPQ", dest, kWatchRegMax);
  char blob[160];
  rebuildWatchRegBlob(dest, n, blob, sizeof(blob));
  TEST_ASSERT_EQUAL_STRING("N2136U,CS-TPQ", blob);
  TEST_ASSERT_TRUE(isValidWatchReg("G-ABCD"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_normalize_us_and_hyphenated);
  RUN_TEST(test_reject_invalid);
  RUN_TEST(test_parse_dedupe_hyphen_insensitive);
  RUN_TEST(test_contains_hyphen_insensitive);
  RUN_TEST(test_rebuild_blob);
  return UNITY_END();
}
