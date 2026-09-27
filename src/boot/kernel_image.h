#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace kestrel {

// Header of an uncompressed arm64 Linux `Image` (Documentation/arch/arm64/booting.rst).
struct Arm64ImageInfo {
  uint64_t text_offset;
  uint64_t image_size;  // memory the kernel needs, including BSS
};

Result<Arm64ImageInfo> parse_arm64_image(std::span<const uint8_t> image);

// Accepts a raw Image, a gzip-compressed Image, or an EFI zboot (`zimg`) image
// with a gzip payload, and returns the raw Image.
Result<std::vector<uint8_t>> unpack_kernel(std::vector<uint8_t> file);

Result<std::vector<uint8_t>> gunzip(std::span<const uint8_t> data);

Result<std::vector<uint8_t>> read_file(const std::string& path);

}  // namespace kestrel
