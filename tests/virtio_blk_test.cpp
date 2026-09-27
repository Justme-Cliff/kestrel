#include "devices/virtio/virtio_blk.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "devices/virtio/virtio_mmio.h"
#include "virtio_test_driver.h"

using namespace kestrel;
using test::QueueBuilder;

namespace {

class MemoryBlockFile : public BlockFile {
 public:
  explicit MemoryBlockFile(size_t size) : data(size, 0) {}
  uint64_t size() const override { return data.size(); }
  bool read_only() const override { return false; }
  bool read_at(uint64_t off, std::span<uint8_t> out) override {
    if (off + out.size() > data.size()) return false;
    std::memcpy(out.data(), data.data() + off, out.size());
    return true;
  }
  bool write_at(uint64_t off, std::span<const uint8_t> in) override {
    if (off + in.size() > data.size()) return false;
    std::memcpy(data.data() + off, in.data(), in.size());
    return true;
  }
  bool flush() override { return true; }
  std::vector<uint8_t> data;
};

struct BlkFixture : ::testing::Test {
  GuestMemory mem = std::move(*GuestMemory::create(Gpa(QueueBuilder::kBase), 1 << 20));
  QueueBuilder q{mem};
  MemoryBlockFile* disk = nullptr;
  std::unique_ptr<VirtioMmio> dev;

  void SetUp() override {
    auto file = std::make_unique<MemoryBlockFile>(64 * 512);
    disk = file.get();
    dev = std::make_unique<VirtioMmio>(mem, std::make_unique<VirtioBlk>(std::move(file)),
                                       [](bool) {});
    // Driver init sequence (virtio 1.2 §3.1.1).
    EXPECT_EQ(dev->mmio_read(0x000, 4), 0x74726976u);
    EXPECT_EQ(dev->mmio_read(0x008, 4), 2u);
    dev->mmio_write(0x070, 4, 1 | 2);
    dev->mmio_write(0x024, 4, 1);
    dev->mmio_write(0x020, 4, 1);  // VIRTIO_F_VERSION_1 (bit 32)
    dev->mmio_write(0x070, 4, 1 | 2 | 8);
    ASSERT_EQ(dev->mmio_read(0x070, 4) & 8, 8u);
    dev->mmio_write(0x030, 4, 0);
    dev->mmio_write(0x038, 4, QueueBuilder::kSize);
    dev->mmio_write(0x080, 4, static_cast<uint32_t>(QueueBuilder::kDesc));
    dev->mmio_write(0x090, 4, static_cast<uint32_t>(QueueBuilder::kAvail));
    dev->mmio_write(0x0a0, 4, static_cast<uint32_t>(QueueBuilder::kUsed));
    dev->mmio_write(0x044, 4, 1);
    dev->mmio_write(0x070, 4, 1 | 2 | 8 | 4);
  }

  void wait_used(uint16_t n) {
    for (int i = 0; i < 1000 && q.used_idx() < n; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(q.used_idx(), n);
  }

  void header(uint64_t addr, uint32_t type, uint64_t sector) {
    uint8_t h[16] = {};
    std::memcpy(h, &type, 4);
    std::memcpy(h + 8, &sector, 8);
    mem.write(Gpa(addr), h);
  }
};

}  // namespace

TEST_F(BlkFixture, ReportsCapacity) {
  EXPECT_EQ(dev->mmio_read(0x100, 4), 64u);
  EXPECT_EQ(dev->mmio_read(0x104, 4), 0u);
}

TEST_F(BlkFixture, WritesThenReadsSector) {
  const uint64_t hdr = QueueBuilder::kData, data = hdr + 0x100, status = hdr + 0x400;
  std::vector<uint8_t> pattern(512);
  for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i * 7);

  header(hdr, /*OUT*/ 1, 3);
  mem.write(Gpa(data), pattern);
  q.submit({{hdr, 16, false}, {data, 512, false}, {status, 1, true}});
  dev->mmio_write(0x050, 4, 0);
  wait_used(1);
  EXPECT_EQ(std::memcmp(disk->data.data() + 3 * 512, pattern.data(), 512), 0);
  EXPECT_EQ(q.get16(status) & 0xff, 0);

  const uint64_t rdata = hdr + 0x1000;
  header(hdr, /*IN*/ 0, 3);
  q.submit({{hdr, 16, false}, {rdata, 512, true}, {status, 1, true}});
  dev->mmio_write(0x050, 4, 0);
  wait_used(2);
  std::vector<uint8_t> back(512);
  mem.read(Gpa(rdata), back);
  EXPECT_EQ(back, pattern);
  EXPECT_EQ(q.used_len(1), 513u);
}

TEST_F(BlkFixture, RejectsReadPastEnd) {
  const uint64_t hdr = QueueBuilder::kData, data = hdr + 0x100, status = hdr + 0x400;
  header(hdr, /*IN*/ 0, 64);
  q.submit({{hdr, 16, false}, {data, 512, true}, {status, 1, true}});
  dev->mmio_write(0x050, 4, 0);
  wait_used(1);
  EXPECT_EQ(q.get16(status) & 0xff, 1);  // VIRTIO_BLK_S_IOERR
}

TEST_F(BlkFixture, RefusesUnofferedFeatures) {
  dev->mmio_write(0x070, 4, 0);  // reset
  dev->mmio_write(0x070, 4, 1 | 2);
  dev->mmio_write(0x024, 4, 0);
  dev->mmio_write(0x020, 4, 1u << 28);  // INDIRECT_DESC: not offered
  dev->mmio_write(0x024, 4, 1);
  dev->mmio_write(0x020, 4, 1);
  dev->mmio_write(0x070, 4, 1 | 2 | 8);
  EXPECT_EQ(dev->mmio_read(0x070, 4) & 8, 0u);
}
