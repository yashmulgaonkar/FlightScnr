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

}  // namespace services::ota
