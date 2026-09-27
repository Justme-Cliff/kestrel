#include "devices/virtio/virtio_pci.h"

#include <gtest/gtest.h>

#include "devices/virtio/virtio_rng.h"

using namespace kestrel;

namespace {
struct PciFixture : ::testing::Test {
  GuestMemory mem = std::move(*GuestMemory::create(Gpa(0x40000000), 1 << 20));
  bool irq = false;
  PciHost host{0x10000000, 0x800000000, [&](unsigned, bool level) { irq = level; }};
  unsigned slot = host.add(std::make_unique<VirtioPci>(
      mem, std::make_unique<VirtioRng>([](std::span<uint8_t> out) { std::ranges::fill(out, 7); })));

  uint64_t cfg(uint16_t reg, unsigned size = 4) {
    return host.ecam().mmio_read((uint64_t(slot) << 15) | reg, size);
  }
  void cfg_write(uint16_t reg, uint32_t v, unsigned size = 4) {
    host.ecam().mmio_write((uint64_t(slot) << 15) | reg, size, v);
  }
};
}  // namespace

TEST_F(PciFixture, IdentifiesAsModernVirtio) {
  EXPECT_EQ(slot, 1u);
  EXPECT_EQ(cfg(0x00, 2), 0x1af4u);
  EXPECT_EQ(cfg(0x02, 2), 0x1040u + 4);  // virtio-rng
  EXPECT_EQ(cfg(0x08, 1), 1u);           // revision 1: modern
  EXPECT_EQ(host.ecam().mmio_read(5ull << 15, 4), 0xffffffffu);  // empty slot
}

TEST_F(PciFixture, BarSizingAndDecode) {
  cfg_write(0x10, 0xffffffff);
  cfg_write(0x14, 0xffffffff);
  EXPECT_EQ(cfg(0x10), (~uint32_t(VirtioPci::kBarSize - 1)) | 0x4);  // 64-bit memory BAR
  EXPECT_EQ(cfg(0x14), 0xffffffffu);
  cfg_write(0x10, 0x10004000);
  cfg_write(0x14, 0);
  // Memory decoding is off until the command register enables it.
  EXPECT_EQ(host.window().mmio_read(0x4000 + 0x12, 2), 0u);
  cfg_write(0x04, 0x2, 2);
  EXPECT_EQ(host.window().mmio_read(0x4000 + 0x12, 2), 1u);  // num_queues
}

TEST_F(PciFixture, CapabilityListHasVirtioStructures) {
  EXPECT_TRUE(cfg(0x06, 2) & 0x10);
  unsigned ptr = static_cast<unsigned>(cfg(0x34, 1));
  std::vector<unsigned> types;
  while (ptr) {
    EXPECT_EQ(cfg(static_cast<uint16_t>(ptr), 1), 0x09u);
    types.push_back(static_cast<unsigned>(cfg(static_cast<uint16_t>(ptr + 3), 1)));
    ptr = static_cast<unsigned>(cfg(static_cast<uint16_t>(ptr + 1), 1));
  }
  EXPECT_EQ(types, (std::vector<unsigned>{1, 3, 4, 2}));
}
