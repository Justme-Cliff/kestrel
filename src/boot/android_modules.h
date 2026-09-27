#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace kestrel {

// Reads `out.size()` bytes at `offset` of the super partition image; false on error.
using SuperReader = std::function<bool(uint64_t offset, std::span<uint8_t> out)>;

// Returns an LZ4-legacy-wrapped cpio (empty if nothing to add).
//
// Android ships some kernel modules it never loads on its own. Cuttlefish only loads
// virtio_balloon when its launcher asks for it, so on Kestrel the guest never gave
// freed RAM back to macOS. This builds a small cpio archive, appended after the
// vendor ramdisk, that adds `modules` (taken unmodified from the system_dlkm
// partition, so they are Google-signed) to first-stage init's load list.
//
// `vendor_ramdisk` is the (LZ4 legacy compressed) vendor ramdisk, read for its
// modules.load / modules.dep. Every size and offset in the images is checked: a
// corrupt image fails this step instead of breaking the boot.
Result<std::vector<uint8_t>> build_extra_modules_cpio(const SuperReader& super,
                                                      std::span<const uint8_t> vendor_ramdisk,
                                                      const std::vector<std::string>& modules);

// Building blocks, exposed for tests.
namespace android_modules {
// LZ4 block format (no frame); fails on malformed input or if more than `max_out` bytes.
Result<std::vector<uint8_t>> lz4_block_decompress(std::span<const uint8_t> in, size_t max_out);
// LZ4 "legacy" frame (magic 0x184C2102), as used for Android ramdisks.
Result<std::vector<uint8_t>> lz4_legacy_decompress(std::span<const uint8_t> in);
struct CpioFile {
  std::string name;  // without leading "/"
  uint32_t mode;
  std::vector<uint8_t> data;
};
// Wraps data in an LZ4 legacy stream without compressing it (data >= 15 bytes).
std::vector<uint8_t> lz4_legacy_store(std::span<const uint8_t> data);
// newc cpio: reads regular files and directories (others are skipped).
Result<std::vector<CpioFile>> cpio_read(std::span<const uint8_t> archive);
std::vector<uint8_t> cpio_write(const std::vector<CpioFile>& files);
// Reads a file from an erofs image (plain, inline or LZ4-compressed, compact or full
// indexes) through `read(offset, out)`.
Result<std::vector<uint8_t>> erofs_read_file(const SuperReader& read, uint64_t image_size,
                                             const std::string& path);
// Byte offset and size of a logical partition (e.g. "system_dlkm_a") in a super image.
struct Extent {
  uint64_t offset, size;
};
Result<Extent> super_partition(const SuperReader& read, const std::string& name);
}  // namespace android_modules

}  // namespace kestrel
