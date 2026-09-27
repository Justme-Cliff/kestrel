#include "devices/pci/pci.h"

#include <cstring>

#include "core/log.h"

namespace kestrel {
namespace {

constexpr uint16_t kCommand = 0x04;
constexpr uint16_t kStatus = 0x06;
constexpr uint16_t kBar0 = 0x10;
constexpr uint16_t kCapPtr = 0x34;
constexpr uint16_t kIntLine = 0x3c;
constexpr uint16_t kIntPin = 0x3d;

constexpr uint16_t kCmdMemory = 1u << 1;
constexpr uint16_t kCmdBusMaster = 1u << 2;
constexpr uint16_t kCmdIntxDisable = 1u << 10;
constexpr uint16_t kStatusCapList = 1u << 4;
constexpr uint32_t kBarMem64 = 0x4;
constexpr uint32_t kBarPrefetchable = 0x8;

void put(std::array<uint8_t, 256>& c, size_t off, uint64_t v, unsigned size) {
  std::memcpy(&c[off], &v, size);  // PCI config space is little-endian, like the host
}
uint32_t get(const std::array<uint8_t, 256>& c, size_t off, unsigned size) {
  uint32_t v = 0;
  std::memcpy(&v, &c[off], size);
  return v;
}

}  // namespace

PciFunction::PciFunction(const Ids& ids) {
  put(config_, 0x00, ids.vendor, 2);
  put(config_, 0x02, ids.device, 2);
  config_[0x08] = ids.revision;
  put(config_, 0x09, ids.class_code, 3);
  config_[0x0e] = 0;  // header type 0, single function
  put(config_, 0x2c, ids.subsystem_vendor, 2);
  put(config_, 0x2e, ids.subsystem, 2);
  config_[kIntPin] = 1;  // INTA#
}

void PciFunction::add_bar64(unsigned index, uint64_t size, bool prefetchable) {
  bar_size_[index] = size;  // must be a power of two
  bar_flags_[index] = kBarMem64 | (prefetchable ? kBarPrefetchable : 0);
  put(config_, kBar0 + 4 * index, bar_flags_[index], 4);
  put(config_, kBar0 + 4 * (index + 1), 0, 4);
}

uint8_t PciFunction::add_capability(uint8_t id, std::vector<uint8_t> body) {
  const uint8_t offset = next_cap_;
  config_[offset] = id;
  config_[offset + 1] = 0;
  std::memcpy(&config_[offset + 2], body.data(), body.size());
  if (last_cap_) {
    config_[last_cap_ + 1] = offset;
  } else {
    config_[kCapPtr] = offset;
    put(config_, kStatus, kStatusCapList, 2);
  }
  last_cap_ = offset;
  next_cap_ = static_cast<uint8_t>((offset + 2 + body.size() + 3) & ~3u);
  return offset;
}

uint64_t PciFunction::bar_address(unsigned index) const {
  const uint64_t lo = get(config_, kBar0 + 4 * index, 4) & ~0xfull;
  const uint64_t hi = get(config_, kBar0 + 4 * (index + 1), 4);
  return (hi << 32) | lo;
}

uint32_t PciFunction::config_read(uint16_t offset, unsigned size) {
  std::lock_guard lock(mutex_);
  if (offset + size > config_.size()) return 0;  // extended config space: nothing there
  return get(config_, offset, size);
}

void PciFunction::config_write(uint16_t offset, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  if (offset + size > config_.size()) return;
  // Only writable fields are updated; everything else is read-only.
  if (offset == kCommand && size >= 2) {
    const uint16_t mask = kCmdMemory | kCmdBusMaster | kCmdIntxDisable;
    put(config_, kCommand, value & mask, 2);
    return;
  }
  if (offset >= kBar0 && offset < kBar0 + 4 * kNumBars && size == 4) {
    const unsigned index = (offset - kBar0) / 4;
    if (bar_size_[index]) {  // low half: keep the type bits, clear the size bits
      put(config_, offset, (value & ~uint32_t(bar_size_[index] - 1) & ~0xfu) | bar_flags_[index], 4);
    } else if (index > 0 && bar_size_[index - 1]) {  // high half of a 64-bit BAR
      const uint64_t size_hi_mask = ~((bar_size_[index - 1] - 1) >> 32);
      put(config_, offset, value & static_cast<uint32_t>(size_hi_mask), 4);
    }
    return;
  }
  if (offset == kIntLine && size == 1) config_[kIntLine] = static_cast<uint8_t>(value);
}

std::optional<uint64_t> PciFunction::bar_base(unsigned index) {
  std::lock_guard lock(mutex_);
  if (index >= kNumBars || !bar_size_[index] || !(get(config_, kCommand, 2) & kCmdMemory)) {
    return std::nullopt;
  }
  const uint64_t base = bar_address(index);
  if (base == 0) return std::nullopt;
  return base;
}

bool PciFunction::decode(uint64_t addr, unsigned size, unsigned& bar, uint64_t& offset) {
  std::lock_guard lock(mutex_);
  if (!(get(config_, kCommand, 2) & kCmdMemory)) return false;
  for (unsigned i = 0; i < kNumBars; ++i) {
    if (!bar_size_[i]) continue;
    const uint64_t base = bar_address(i);
    if (base == 0) continue;  // not assigned yet
    if (addr >= base && addr - base + size <= bar_size_[i]) {
      bar = i;
      offset = addr - base;
      return true;
    }
  }
  return false;
}

void PciFunction::set_intx(bool level) {
  bool disabled;
  {
    std::lock_guard lock(mutex_);
    disabled = get(config_, kCommand, 2) & kCmdIntxDisable;
    if (level == intx_level_) return;
    intx_level_ = level;
  }
  if (intx_sink_) intx_sink_(level && !disabled);
}

PciHost::PciHost(uint64_t window_base, uint64_t window64_base,
                 std::function<void(unsigned, bool)> irq)
    : irq_(std::move(irq)), ecam_(*this), window_(*this, window_base), window64_(*this, window64_base) {}

unsigned PciHost::add(std::unique_ptr<PciFunction> function) {
  const unsigned slot = static_cast<unsigned>(functions_.size()) + 1;  // 00:00.0 left empty
  function->set_intx_sink([this, slot](bool level) { irq_(slot, level); });
  functions_.emplace_back(slot, std::move(function));
  return slot;
}

std::vector<unsigned> PciHost::slots() const {
  std::vector<unsigned> s;
  for (const auto& [slot, f] : functions_) s.push_back(slot);
  return s;
}

PciFunction* PciHost::at_slot(unsigned slot) {
  for (auto& [s, f] : functions_) {
    if (s == slot) return f.get();
  }
  return nullptr;
}

uint64_t PciHost::Ecam::mmio_read(uint64_t offset, unsigned size) {
  // ECAM address: bus[27:20] device[19:15] function[14:12] register[11:0].
  const unsigned bus = (offset >> 20) & 0xff, dev = (offset >> 15) & 0x1f, fn = (offset >> 12) & 7;
  PciFunction* f = (bus == 0 && fn == 0) ? host_.at_slot(dev) : nullptr;
  if (!f) return size == 4 ? 0xffffffffu : (1ull << (size * 8)) - 1;  // no device
  return f->config_read(static_cast<uint16_t>(offset & 0xfff), size);
}

void PciHost::Ecam::mmio_write(uint64_t offset, unsigned size, uint64_t value) {
  const unsigned bus = (offset >> 20) & 0xff, dev = (offset >> 15) & 0x1f, fn = (offset >> 12) & 7;
  PciFunction* f = (bus == 0 && fn == 0) ? host_.at_slot(dev) : nullptr;
  if (f) f->config_write(static_cast<uint16_t>(offset & 0xfff), size, static_cast<uint32_t>(value));
}

uint64_t PciHost::Window::mmio_read(uint64_t offset, unsigned size) {
  const uint64_t addr = base_ + offset;
  for (auto& [slot, f] : host_.functions_) {
    unsigned bar;
    uint64_t off;
    if (f->decode(addr, size, bar, off)) return f->bar_read(bar, off, size);
  }
  return 0;
}

void PciHost::Window::mmio_write(uint64_t offset, unsigned size, uint64_t value) {
  const uint64_t addr = base_ + offset;
  for (auto& [slot, f] : host_.functions_) {
    unsigned bar;
    uint64_t off;
    if (f->decode(addr, size, bar, off)) {
      f->bar_write(bar, off, size, value);
      return;
    }
  }
}

}  // namespace kestrel
