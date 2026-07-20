#include <unity.h>

#include "services/firmware_image.h"

using services::ota::firmwareHeaderLooksValid;
using services::ota::kEspImageMagic;
using services::ota::kMinImageSize;

void setUp(void) {}
void tearDown(void) {}

// Target OTA app partition size on the T-Encoder Pro (0x640000 = 6.5 MB).
static constexpr size_t kPart = 0x640000u;

// (a) valid: magic byte + a size comfortably within the partition.
void test_valid_image(void) {
  const uint8_t head[] = {kEspImageMagic, 0x00, 0x03, 0x02};
  TEST_ASSERT_TRUE(firmwareHeaderLooksValid(head, sizeof(head),
                                            5u * 1024u * 1024u, kPart));
}

// (b) wrong magic byte -> rejected regardless of size.
void test_wrong_magic(void) {
  const uint8_t head[] = {0x1F, 0x8B, 0x08};  // gzip, not an ESP image
  TEST_ASSERT_FALSE(firmwareHeaderLooksValid(head, sizeof(head),
                                             5u * 1024u * 1024u, kPart));
}

// (c) total_size unknown (0): only the magic byte decides.
void test_unknown_total_size(void) {
  const uint8_t good[] = {kEspImageMagic, 0x00};
  const uint8_t bad[] = {0x00, 0x00};
  TEST_ASSERT_TRUE(firmwareHeaderLooksValid(good, sizeof(good), 0, kPart));
  TEST_ASSERT_FALSE(firmwareHeaderLooksValid(bad, sizeof(bad), 0, kPart));
}

// (d) too big: total_size exceeds the partition -> rejected.
void test_too_big(void) {
  const uint8_t head[] = {kEspImageMagic};
  TEST_ASSERT_FALSE(firmwareHeaderLooksValid(head, sizeof(head), kPart + 1, kPart));
}

// boundary: exactly partition-sized is accepted.
void test_exact_partition_size(void) {
  const uint8_t head[] = {kEspImageMagic};
  TEST_ASSERT_TRUE(firmwareHeaderLooksValid(head, sizeof(head), kPart, kPart));
}

// (e) too small: below the plausible minimum -> rejected.
void test_too_small(void) {
  const uint8_t head[] = {kEspImageMagic};
  TEST_ASSERT_FALSE(firmwareHeaderLooksValid(head, sizeof(head), kMinImageSize - 1, kPart));
  // boundary: exactly the minimum is accepted.
  TEST_ASSERT_TRUE(firmwareHeaderLooksValid(head, sizeof(head), kMinImageSize, kPart));
}

// (f) head_len == 0 (or null) -> rejected: nothing to validate.
void test_empty_head(void) {
  const uint8_t head[] = {kEspImageMagic};
  TEST_ASSERT_FALSE(firmwareHeaderLooksValid(head, 0, 5u * 1024u * 1024u, kPart));
  TEST_ASSERT_FALSE(firmwareHeaderLooksValid(nullptr, 4, 5u * 1024u * 1024u, kPart));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_image);
  RUN_TEST(test_wrong_magic);
  RUN_TEST(test_unknown_total_size);
  RUN_TEST(test_too_big);
  RUN_TEST(test_exact_partition_size);
  RUN_TEST(test_too_small);
  RUN_TEST(test_empty_head);
  return UNITY_END();
}
