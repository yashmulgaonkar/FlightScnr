#include <unity.h>

#include "services/firmware_image.h"

using services::ota::firmwareHeaderLooksValid;
using services::ota::firmwareSizeLooksValid;
using services::ota::kAppDescMagic;
using services::ota::kAppDescOffset;
using services::ota::kEspImageMagic;
using services::ota::kMinImageSize;

void setUp(void) {}
void tearDown(void) {}

// Target OTA app partition size on the T-Encoder Pro (0x640000 = 6.5 MB).
static constexpr size_t kPart = 0x640000u;

// A realistic app-image head: 0xE9 at offset 0 and the esp_app_desc magic
// (little-endian) at offset 0x20 — like the real first WRITE chunk (~1.4-2 KB).
static uint8_t s_app_head[64];
static void buildAppHead(bool with_desc_magic) {
  for (size_t i = 0; i < sizeof(s_app_head); ++i) s_app_head[i] = 0x00;
  s_app_head[0] = kEspImageMagic;
  if (with_desc_magic) {
    s_app_head[kAppDescOffset + 0] = static_cast<uint8_t>(kAppDescMagic & 0xFF);
    s_app_head[kAppDescOffset + 1] = static_cast<uint8_t>((kAppDescMagic >> 8) & 0xFF);
    s_app_head[kAppDescOffset + 2] = static_cast<uint8_t>((kAppDescMagic >> 16) & 0xFF);
    s_app_head[kAppDescOffset + 3] = static_cast<uint8_t>((kAppDescMagic >> 24) & 0xFF);
  }
}

// (a) valid: full-length app head with 0xE9 + app-desc magic at 0x20.
void test_valid_image(void) {
  buildAppHead(/*with_desc_magic=*/true);
  TEST_ASSERT_TRUE(firmwareHeaderLooksValid(s_app_head, sizeof(s_app_head),
                                            5u * 1024u * 1024u, kPart));
}

// (a2) merged/bootloader image: starts with 0xE9 and fits, but has NO app-desc
// magic at 0x20 -> rejected (would corrupt the app partition if flashed).
void test_merged_image_rejected(void) {
  buildAppHead(/*with_desc_magic=*/false);
  TEST_ASSERT_FALSE(firmwareHeaderLooksValid(s_app_head, sizeof(s_app_head),
                                             5u * 1024u * 1024u, kPart));
}

// (a3) short head (< 0x24 bytes): descriptor check is skipped, so a bare 0xE9
// in-range head still passes (real first chunk is always long enough).
void test_short_head_skips_desc_check(void) {
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

// --- firmwareSizeLooksValid (applied at UPLOAD_FILE_END, size is final) ---

// (g) size in range -> valid.
void test_size_valid(void) {
  TEST_ASSERT_TRUE(firmwareSizeLooksValid(5u * 1024u * 1024u, kPart));
}

// (h) size 0 (never a real image) -> rejected. Unlike the header check, there
// is no "unknown" escape hatch here: at END the total is authoritative.
void test_size_zero_rejected(void) {
  TEST_ASSERT_FALSE(firmwareSizeLooksValid(0, kPart));
}

// (i) too small -> rejected; exactly the minimum -> accepted.
void test_size_too_small(void) {
  TEST_ASSERT_FALSE(firmwareSizeLooksValid(kMinImageSize - 1, kPart));
  TEST_ASSERT_TRUE(firmwareSizeLooksValid(kMinImageSize, kPart));
}

// (j) too big -> rejected; exactly partition-sized -> accepted.
void test_size_too_big(void) {
  TEST_ASSERT_FALSE(firmwareSizeLooksValid(kPart + 1, kPart));
  TEST_ASSERT_TRUE(firmwareSizeLooksValid(kPart, kPart));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_image);
  RUN_TEST(test_merged_image_rejected);
  RUN_TEST(test_short_head_skips_desc_check);
  RUN_TEST(test_wrong_magic);
  RUN_TEST(test_unknown_total_size);
  RUN_TEST(test_too_big);
  RUN_TEST(test_exact_partition_size);
  RUN_TEST(test_too_small);
  RUN_TEST(test_empty_head);
  RUN_TEST(test_size_valid);
  RUN_TEST(test_size_zero_rejected);
  RUN_TEST(test_size_too_small);
  RUN_TEST(test_size_too_big);
  return UNITY_END();
}
