#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "core/error.h"

namespace kestrel {

// A host file used as a disk. Per-OS implementation in src/host/<os>/.
class BlockFile {
 public:
  static Result<std::unique_ptr<BlockFile>> open(const std::string& path, bool read_only);
  virtual ~BlockFile() = default;
  virtual uint64_t size() const = 0;
  virtual bool read_only() const = 0;
  virtual bool read_at(uint64_t offset, std::span<uint8_t> out) = 0;
  virtual bool write_at(uint64_t offset, std::span<const uint8_t> in) = 0;
  virtual bool flush() = 0;
};

}  // namespace kestrel
