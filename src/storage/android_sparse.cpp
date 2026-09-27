#include "storage/android_sparse.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

namespace kestrel {
namespace {

constexpr uint32_t kMagic = 0xed26ff3a;
constexpr uint16_t kChunkRaw = 0xcac1;
constexpr uint16_t kChunkFill = 0xcac2;
constexpr uint16_t kChunkDontCare = 0xcac3;
constexpr uint16_t kChunkCrc32 = 0xcac4;

struct FileHeader {
  uint32_t magic;
  uint16_t major, minor;
  uint16_t file_hdr_sz, chunk_hdr_sz;
  uint32_t blk_sz, total_blks, total_chunks, image_checksum;
};
struct ChunkHeader {
  uint16_t chunk_type, reserved;
  uint32_t chunk_sz;  // in blocks
  uint32_t total_sz;  // bytes, including this header
};
static_assert(sizeof(FileHeader) == 28 && sizeof(ChunkHeader) == 12);

}  // namespace

bool is_android_sparse(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  uint32_t magic = 0;
  in.read(reinterpret_cast<char*>(&magic), 4);
  return in && magic == kMagic;
}

Result<void> unsparse_image(const std::string& in_path, const std::string& out_path) {
  std::ifstream in(in_path, std::ios::binary);
  if (!in) return make_error("cannot open " + in_path);
  FileHeader h{};
  in.read(reinterpret_cast<char*>(&h), sizeof(h));
  if (!in || h.magic != kMagic) {
    std::error_code ec;
    std::filesystem::copy_file(in_path, out_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return make_error("copy failed: " + ec.message());
    return {};
  }
  if (h.major != 1 || h.file_hdr_sz < sizeof(FileHeader) || h.chunk_hdr_sz < sizeof(ChunkHeader) ||
      h.blk_sz == 0 || h.blk_sz % 4 != 0) {
    return make_error("unsupported sparse image header in " + in_path);
  }
  in.seekg(h.file_hdr_sz);

  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) return make_error("cannot create " + out_path);
  const uint64_t total_bytes = uint64_t(h.total_blks) * h.blk_sz;
  uint64_t pos = 0;
  std::vector<char> buf;
  for (uint32_t i = 0; i < h.total_chunks; ++i) {
    ChunkHeader c{};
    in.read(reinterpret_cast<char*>(&c), sizeof(c));
    if (!in) return make_error(std::format("truncated sparse image at chunk {}", i));
    in.seekg(h.chunk_hdr_sz - sizeof(ChunkHeader), std::ios::cur);
    const uint64_t bytes = uint64_t(c.chunk_sz) * h.blk_sz;
    const uint64_t payload = c.total_sz - h.chunk_hdr_sz;
    if (pos + bytes > total_bytes) return make_error("sparse chunk runs past the image end");
    switch (c.chunk_type) {
      case kChunkRaw:
        if (payload != bytes) return make_error("bad raw chunk size");
        buf.resize(1 << 20);
        out.seekp(static_cast<std::streamoff>(pos));
        for (uint64_t left = bytes; left > 0;) {
          const auto n = static_cast<std::streamsize>(std::min<uint64_t>(left, buf.size()));
          in.read(buf.data(), n);
          if (!in) return make_error("truncated raw chunk");
          out.write(buf.data(), n);
          left -= static_cast<uint64_t>(n);
        }
        break;
      case kChunkFill: {
        uint32_t fill = 0;
        in.read(reinterpret_cast<char*>(&fill), 4);
        if (fill != 0) {  // zero fills stay holes
          buf.resize(h.blk_sz);
          for (size_t k = 0; k < buf.size(); k += 4) std::memcpy(&buf[k], &fill, 4);
          out.seekp(static_cast<std::streamoff>(pos));
          for (uint32_t b = 0; b < c.chunk_sz; ++b) out.write(buf.data(), h.blk_sz);
        }
        break;
      }
      case kChunkDontCare:
        break;
      case kChunkCrc32:
        in.seekg(static_cast<std::streamoff>(payload), std::ios::cur);
        break;
      default:
        return make_error(std::format("unknown sparse chunk type {:#x}", c.chunk_type));
    }
    pos += bytes;
  }
  out.close();
  // Extend to the full size; the tail stays a hole.
  std::error_code ec;
  std::filesystem::resize_file(out_path, total_bytes, ec);
  if (ec) return make_error("resize failed: " + ec.message());
  return {};
}

}  // namespace kestrel
