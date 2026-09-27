#include "core/mmio_bus.h"

#include <gtest/gtest.h>

using namespace kestrel;

namespace {
class FakeDevice : public MmioDevice {
 public:
  std::string_view name() const override { return "fake"; }
  uint64_t mmio_read(uint64_t offset, unsigned) override { return 0x1000 + offset; }
  void mmio_write(uint64_t offset, unsigned, uint64_t value) override {
    last_offset = offset;
    last_value = value;
  }
  uint64_t last_offset = 0, last_value = 0;
};
}  // namespace

TEST(MmioBus, DispatchesByOffset) {
  MmioBus bus;
  FakeDevice d;
  ASSERT_TRUE(bus.add(Gpa(0x9000000), 0x1000, &d));
  EXPECT_EQ(bus.read(Gpa(0x9000018), 4), 0x1018u);
  EXPECT_TRUE(bus.write(Gpa(0x9000004), 4, 42));
  EXPECT_EQ(d.last_offset, 4u);
  EXPECT_EQ(d.last_value, 42u);
}

TEST(MmioBus, UnmappedAndStraddling) {
  MmioBus bus;
  FakeDevice d;
  ASSERT_TRUE(bus.add(Gpa(0x9000000), 0x1000, &d));
  EXPECT_FALSE(bus.read(Gpa(0x8fffffc), 4));
  EXPECT_FALSE(bus.read(Gpa(0x9001000), 4));
  EXPECT_FALSE(bus.read(Gpa(0x9000ffe), 4));  // crosses the end
}

TEST(MmioBus, RejectsOverlap) {
  MmioBus bus;
  FakeDevice a, b;
  ASSERT_TRUE(bus.add(Gpa(0x1000), 0x1000, &a));
  EXPECT_FALSE(bus.add(Gpa(0x1800), 0x1000, &b));
  EXPECT_FALSE(bus.add(Gpa(0x0800), 0x1000, &b));
  EXPECT_TRUE(bus.add(Gpa(0x2000), 0x1000, &b));
  EXPECT_FALSE(bus.add(Gpa(~0ull - 10), 0x1000, &b));
}
