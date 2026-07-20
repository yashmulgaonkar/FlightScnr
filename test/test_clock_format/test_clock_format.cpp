#include <unity.h>

#include <cstring>
#include <ctime>

#include "services/clock_format.h"

using services::clock::formatCivilDate;

void setUp(void) {}
void tearDown(void) {}

// Fully-populated local time for Monday, 2026-07-20.
// %a reads tm_wday directly (not derived), so it must be set explicitly.
static struct tm mondayJul20(void) {
  struct tm t {};
  t.tm_year = 126;  // 1900 + 126 = 2026
  t.tm_mon = 6;     // July (0-based)
  t.tm_mday = 20;
  t.tm_wday = 1;    // Monday
  t.tm_hour = 12;
  t.tm_min = 34;
  t.tm_sec = 56;
  return t;
}

void test_text_format_default(void) {
  struct tm t = mondayJul20();
  char out[32] = "xxx";
  formatCivilDate(t, false, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Mon, Jul 20", out);
}

void test_numeric_format(void) {
  struct tm t = mondayJul20();
  char out[32] = "xxx";
  formatCivilDate(t, true, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("20.07.2026", out);
}

void test_numeric_zero_padding(void) {
  struct tm t {};
  t.tm_year = 126;  // 2026
  t.tm_mon = 0;     // January
  t.tm_mday = 5;
  t.tm_wday = 1;
  char out[32] = "xxx";
  formatCivilDate(t, true, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("05.01.2026", out);
}

void test_short_buffer_no_overflow(void) {
  struct tm t = mondayJul20();
  char guard[16];
  memset(guard, 0x7f, sizeof(guard));
  // "20.07.2026" needs 11 bytes; give strftime only 4 and confirm it never
  // writes past the allowed region (strftime's own size guarantee).
  formatCivilDate(t, true, guard, 4);
  for (size_t i = 4; i < sizeof(guard); ++i) {
    TEST_ASSERT_EQUAL_CHAR(0x7f, guard[i]);
  }
}

void test_null_and_zero_len_guards(void) {
  struct tm t = mondayJul20();
  char out[8] = "keep";
  formatCivilDate(t, true, nullptr, sizeof(out));  // must not crash
  formatCivilDate(t, true, out, 0);                // must not touch out
  TEST_ASSERT_EQUAL_STRING("keep", out);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_text_format_default);
  RUN_TEST(test_numeric_format);
  RUN_TEST(test_numeric_zero_padding);
  RUN_TEST(test_short_buffer_no_overflow);
  RUN_TEST(test_null_and_zero_len_guards);
  return UNITY_END();
}
