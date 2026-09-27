#include "core/mmio_bus.h"

namespace kestrel {

bool MmioBus::add(Gpa base, uint64_t size, MmioDevice* device) {
  if (size == 0 || device == nullptr || base.value + size < base.value) return false;
  auto next = regions_.lower_bound(base.value);
  if (next != regions_.end() && next->first < base.value + size) return false;
  if (next != regions_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second.size > base.value) return false;
  }
  regions_.emplace(base.value, Region{size, device});
  return true;
}

const std::pair<const uint64_t, MmioBus::Region>* MmioBus::find(uint64_t addr,
                                                                unsigned size) const {
  auto it = regions_.upper_bound(addr);
  if (it == regions_.begin()) return nullptr;
  --it;
  const uint64_t offset = addr - it->first;
  if (offset >= it->second.size || size > it->second.size - offset) return nullptr;
  return &*it;
}

std::optional<uint64_t> MmioBus::read(Gpa addr, unsigned size) const {
  auto* region = find(addr.value, size);
  if (!region) return std::nullopt;
  return region->second.device->mmio_read(addr.value - region->first, size);
}

bool MmioBus::write(Gpa addr, unsigned size, uint64_t value) const {
  auto* region = find(addr.value, size);
  if (!region) return false;
  region->second.device->mmio_write(addr.value - region->first, size, value);
  return true;
}

}  // namespace kestrel
