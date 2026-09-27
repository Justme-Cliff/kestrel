#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/error.h"

namespace kestrel {

struct VirtBoardConfig {
  uint64_t ram_base;
  uint64_t ram_size;
  unsigned cpus = 1;
  uint64_t gic_dist_base, gic_dist_size;
  uint64_t gic_redist_base, gic_redist_size;
  std::string cmdline;
  std::optional<std::pair<uint64_t, uint64_t>> initrd;  // [start, end)
  unsigned virtio_slots = 0;         // virtio-mmio slots 0..N-1 in use
  std::vector<unsigned> pci_slots;   // occupied PCI device numbers on bus 0
};

// Device tree describing our board to the guest kernel (docs/DESIGN.md §5.4).
Result<std::vector<uint8_t>> build_virt_dtb(const VirtBoardConfig& cfg);

}  // namespace kestrel
