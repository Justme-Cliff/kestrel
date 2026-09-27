#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "core/gpa.h"
#include "core/mmio_device.h"

namespace kestrel {

// Maps guest physical address ranges to devices. Built before vCPUs start,
// read-only afterwards, so lookups need no locking.
class MmioBus {
 public:
  // Returns false if the range is empty, overflows or overlaps an existing device.
  bool add(Gpa base, uint64_t size, MmioDevice* device);

  // Returns nullopt when no device claims the address (the access is a guest bug).
  std::optional<uint64_t> read(Gpa addr, unsigned size) const;
  bool write(Gpa addr, unsigned size, uint64_t value) const;

 private:
  struct Region {
    uint64_t size;
    MmioDevice* device;
  };
  const std::pair<const uint64_t, Region>* find(uint64_t addr, unsigned size) const;

  std::map<uint64_t, Region> regions_;
};

}  // namespace kestrel
