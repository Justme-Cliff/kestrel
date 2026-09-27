#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/error.h"
#include "host/block_file.h"

namespace kestrel {

// A virtual GPT disk whose partitions are separate host files (like crosvm's
// composite disk). Android finds its partitions by GPT name
// (/dev/block/by-name/<name>), so the images are used as-is, never copied.
class CompositeDisk final : public BlockFile {
 public:
  struct Partition {
    std::string name;  // GPT partition name, e.g. "super" or "boot_a"
    std::unique_ptr<BlockFile> file;
  };

  static Result<std::unique_ptr<CompositeDisk>> create(std::vector<Partition> partitions);

  uint64_t size() const override { return size_; }
  bool read_only() const override { return false; }
  bool read_at(uint64_t offset, std::span<uint8_t> out) override;
  bool write_at(uint64_t offset, std::span<const uint8_t> in) override;
  bool flush() override;

  static constexpr uint64_t kSector = 512;
  static constexpr uint64_t kAlign = 1 << 20;  // partitions start on 1 MiB boundaries

  struct Extent {
    uint64_t start, size;
    BlockFile* file;                 // nullptr: synthesized bytes (or zeros if empty)
    std::vector<uint8_t> synthetic;  // GPT headers
  };
  const std::vector<Extent>& extents() const { return extents_; }

 private:
  CompositeDisk() = default;

  std::vector<Partition> partitions_;
  std::vector<Extent> extents_;  // sorted, non-overlapping; gaps read as zeros
  uint64_t size_ = 0;
};

}  // namespace kestrel
