#include "boot/kernel_image.h"

#include <zlib.h>

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace kestrel {
namespace {

uint32_t le32(std::span<const uint8_t> d, size_t off) {
  uint32_t v;
  std::memcpy(&v, d.data() + off, 4);
  return v;  // host is little-endian (arm64)
}

uint64_t le64(std::span<const uint8_t> d, size_t off) {
  uint64_t v;
  std::memcpy(&v, d.data() + off, 8);
  return v;
}

constexpr size_t kImageHeaderSize = 64;
constexpr uint32_t kArm64Magic = 0x644d5241;  // "ARM\x64" at offset 0x38

}  // namespace

Result<Arm64ImageInfo> parse_arm64_image(std::span<const uint8_t> image) {
  if (image.size() < kImageHeaderSize) return make_error("kernel image too small");
  if (le32(image, 0x38) != kArm64Magic) return make_error("not an arm64 Linux Image (bad magic)");
  Arm64ImageInfo info{.text_offset = le64(image, 8), .image_size = le64(image, 16)};
  // Kernels before 3.17 leave image_size 0; the text_offset then is 0x80000 by convention.
  if (info.image_size == 0) {
    info.text_offset = 0x80000;
    info.image_size = image.size();
  }
  if (info.image_size < image.size()) return make_error("kernel image_size smaller than file");
  return info;
}

Result<std::vector<uint8_t>> gunzip(std::span<const uint8_t> data) {
  z_stream zs{};
  // 15 + 32: auto-detect gzip or zlib headers.
  if (inflateInit2(&zs, 15 + 32) != Z_OK) return make_error("inflateInit2 failed");
  std::vector<uint8_t> out(data.size() * 4 + 1024);
  zs.next_in = const_cast<Bytef*>(data.data());
  zs.avail_in = static_cast<uInt>(data.size());
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    if (zs.total_out == out.size()) out.resize(out.size() * 2);
    zs.next_out = out.data() + zs.total_out;
    zs.avail_out = static_cast<uInt>(out.size() - zs.total_out);
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) {
      inflateEnd(&zs);
      return make_error(std::format("gzip decompression failed (zlib error {})", rc));
    }
  }
  out.resize(zs.total_out);
  inflateEnd(&zs);
  return out;
}

Result<std::vector<uint8_t>> unpack_kernel(std::vector<uint8_t> file) {
  std::span<const uint8_t> d(file);
  if (d.size() >= 2 && d[0] == 0x1f && d[1] == 0x8b) return gunzip(d);

  // EFI zboot: "MZ" PE header with "zimg" at offset 4, payload offset/size at 8/12,
  // compression name at 0x18 (linux/drivers/firmware/efi/libstub/zboot-header.S).
  if (d.size() >= 0x20 && d[0] == 'M' && d[1] == 'Z' && std::memcmp(d.data() + 4, "zimg", 4) == 0) {
    const uint32_t off = le32(d, 8);
    const uint32_t size = le32(d, 12);
    if (off > d.size() || size > d.size() - off) return make_error("corrupt zboot header");
    const std::string comp(reinterpret_cast<const char*>(d.data() + 0x18),
                           strnlen(reinterpret_cast<const char*>(d.data() + 0x18), 32));
    if (comp != "gzip") return make_error("zboot kernel uses '" + comp + "', only gzip supported");
    return gunzip(d.subspan(off, size));
  }
  return file;
}

Result<std::vector<uint8_t>> read_file(const std::string& path) {
  // One read of the whole file: boot images are tens of MB.
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return make_error("cannot open " + path);
  const std::streamoff size = in.tellg();
  if (size < 0) return make_error("cannot size " + path);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(data.data()), size)) return make_error("cannot read " + path);
  return data;
}

}  // namespace kestrel
