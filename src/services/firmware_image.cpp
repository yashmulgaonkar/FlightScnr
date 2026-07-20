#include "services/firmware_image.h"

namespace services::ota {

bool firmwareHeaderLooksValid(const uint8_t* head, size_t head_len,
                              size_t total_size, size_t max_partition_size) {
  if (head == nullptr || head_len < 1) {
    return false;
  }
  if (head[0] != kEspImageMagic) {
    return false;
  }
  // Distinguish a real app image from a merged/full image: an app image carries
  // the esp_app_desc magic (little-endian) at offset 0x20; the merged image
  // starts with the bootloader (also 0xE9) and has no descriptor there. Only
  // enforce when the head is long enough to reach it (real first chunk is).
  if (head_len >= kAppDescOffset + 4) {
    const uint32_t desc = static_cast<uint32_t>(head[kAppDescOffset]) |
                          (static_cast<uint32_t>(head[kAppDescOffset + 1]) << 8) |
                          (static_cast<uint32_t>(head[kAppDescOffset + 2]) << 16) |
                          (static_cast<uint32_t>(head[kAppDescOffset + 3]) << 24);
    if (desc != kAppDescMagic) {
      return false;
    }
  }
  if (total_size > 0) {
    if (total_size > max_partition_size) {
      return false;
    }
    if (total_size < kMinImageSize) {
      return false;
    }
  }
  return true;
}

bool firmwareSizeLooksValid(size_t total_size, size_t max_partition_size) {
  return total_size >= kMinImageSize && total_size <= max_partition_size;
}

}  // namespace services::ota
