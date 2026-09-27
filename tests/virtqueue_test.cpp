#include "devices/virtio/virtqueue.h"

#include <gtest/gtest.h>

#include "virtio_test_driver.h"

using namespace kestrel;
using test::QueueBuilder;

namespace {
struct Fixture : ::testing::Test {
  GuestMemory mem = std::move(*GuestMemory::create(Gpa(QueueBuilder::kBase), 1 << 20));
  QueueBuilder q{mem};
  Virtqueue vq;
  void SetUp() override {
    ASSERT_TRUE(vq.configure(mem, QueueBuilder::kSize, Gpa(QueueBuilder::kDesc),
                             Gpa(QueueBuilder::kAvail), Gpa(QueueBuilder::kUsed)));
  }
};
}  // namespace

TEST_F(Fixture, PopsChainAndCompletes) {
  const uint16_t head = q.submit({{QueueBuilder::kData, 16, false}, {QueueBuilder::kData + 16, 8, true}});
  auto chain = vq.pop();
  ASSERT_TRUE(chain && *chain);
  EXPECT_EQ((*chain)->head, head);
  ASSERT_EQ((*chain)->buffers.size(), 2u);
  EXPECT_FALSE((*chain)->buffers[0].device_writable);
  EXPECT_TRUE((*chain)->buffers[1].device_writable);
  vq.push_used(head, 8);
  EXPECT_EQ(q.used_idx(), 1);
  EXPECT_EQ(q.used_len(0), 8u);
  auto empty = vq.pop();
  ASSERT_TRUE(empty);
  EXPECT_FALSE(*empty);
}

TEST_F(Fixture, RejectsBufferOutsideRam) {
  q.submit({{0x10, 16, false}});
  EXPECT_FALSE(vq.pop());
}

TEST_F(Fixture, RejectsDescriptorLoop) {
  q.write_desc(0, QueueBuilder::kData, 4, /*NEXT*/ 1, 1);
  q.write_desc(1, QueueBuilder::kData, 4, /*NEXT*/ 1, 0);
  q.publish_raw(0);
  EXPECT_FALSE(vq.pop());
}

TEST_F(Fixture, RejectsHeadOutOfRange) {
  q.publish_raw(QueueBuilder::kSize + 3);
  EXPECT_FALSE(vq.pop());
}

TEST_F(Fixture, RejectsIndirect) {
  q.write_desc(0, QueueBuilder::kData, 16, /*INDIRECT*/ 4, 0);
  q.publish_raw(0);
  EXPECT_FALSE(vq.pop());
}

TEST(VirtqueueConfig, RejectsBadLayouts) {
  auto mem = GuestMemory::create(Gpa(QueueBuilder::kBase), 1 << 20);
  ASSERT_TRUE(mem);
  Virtqueue vq;
  EXPECT_FALSE(vq.configure(*mem, 3, Gpa(QueueBuilder::kDesc), Gpa(QueueBuilder::kAvail),
                            Gpa(QueueBuilder::kUsed)));  // not a power of two
  EXPECT_FALSE(vq.configure(*mem, 16, Gpa(QueueBuilder::kDesc + 8), Gpa(QueueBuilder::kAvail),
                            Gpa(QueueBuilder::kUsed)));  // misaligned
  EXPECT_FALSE(vq.configure(*mem, 16, Gpa(0x1000), Gpa(QueueBuilder::kAvail),
                            Gpa(QueueBuilder::kUsed)));  // outside RAM
}
