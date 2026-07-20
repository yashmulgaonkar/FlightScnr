#pragma once

#include <cstddef>
#include <cstdint>

namespace services::ota {

/**
 * Cheap sanity check on the leading bytes of an uploaded OTA image before it is
 * streamed into the inactive app partition.
 *
 * Pure, hardware-free transformation (test seam). No Arduino/ESP deps. It only
 * rejects obviously wrong uploads early (wrong file, truncated, oversized); the
 * cryptographic/structural validation is done by Update.end() on the device.
 *
 * @param head      First bytes received from the upload (must be >= 1 byte).
 * @param head_len  Number of valid bytes in @p head.
 * @param total_size  Announced total upload size, or 0 if unknown.
 * @param max_partition_size  Size of the target OTA app partition (bytes).
 *
 * Rules (all must hold, else false):
 *   - head_len >= 1 and head != nullptr.
 *   - head[0] == 0xE9 (ESP image magic byte).
 *   - if total_size > 0:
 *       total_size <= max_partition_size, and
 *       total_size >= kMinImageSize (a plausible minimum app image).
 *   - total_size == 0 (unknown) is accepted on size grounds; only magic counts.
 */
bool firmwareHeaderLooksValid(const uint8_t* head, size_t head_len,
                              size_t total_size, size_t max_partition_size);

/**
 * Size-only sanity check applied once the *final* upload size is known.
 *
 * On the ESP32 WebServer, upload.totalSize is still 0 during the first
 * UPLOAD_FILE_WRITE callback (it is only incremented after the callback runs),
 * so the size rules inside firmwareHeaderLooksValid never fire mid-stream.
 * This function is meant for UPLOAD_FILE_END, where the total is final.
 *
 * Pure, hardware-free transformation (test seam). No Arduino/ESP deps.
 *
 * Rules (both must hold, else false):
 *   - total_size >= kMinImageSize (a plausible minimum app image).
 *   - total_size <= max_partition_size (fits the OTA app partition).
 */
bool firmwareSizeLooksValid(size_t total_size, size_t max_partition_size);

/** ESP32 image magic byte at offset 0. */
constexpr uint8_t kEspImageMagic = 0xE9;

/** Plausible minimum size of a real app image (64 KB). Smaller uploads are
 *  almost certainly a wrong/truncated file, not the firmware.bin. */
constexpr size_t kMinImageSize = 64u * 1024u;

}  // namespace services::ota
