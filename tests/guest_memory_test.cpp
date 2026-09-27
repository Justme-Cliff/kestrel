#include "core/guest_memory.h"

#include <gtest/gtest.h>

#include <array>

using namespace kestrel;

TEST(GuestMemory, TranslateBounds) {
  auto m = GuestMemory::create(Gpa(0x40000000), 64 * 1024);
  ASSERT_TRUE(m);
  EXPECT_TRUE(m->translate(Gpa(0x40000000), 64 * 1024));
  EXPECT_FALSE(m->translate(Gpa(0x40000000), 64 * 1024 + 1));
  EXPECT_FALSE(m->translate(Gpa(0x3fffffff), 1));
  EXPECT_FALSE(m->translate(Gpa(0x40010000), 1));
  EXPECT_TRUE(m->translate(Gpa(0x40010000), 0));
}

TEST(GuestMemory, RejectsOverflow) {
  auto m = GuestMemory::create(Gpa(0x40000000), 64 * 1024);
  ASSERT_TRUE(m);
  EXPECT_FALSE(m->translate(Gpa(0x40000010), ~0ull));
  EXPECT_FALSE(m->translate(Gpa(~0ull), 2));
}

TEST(GuestMemory, ReadWriteRoundTrip) {
  auto m = GuestMemory::create(Gpa(0x40000000), 16 * 1024);
  ASSERT_TRUE(m);
  std::array<uint8_t, 4> in{1, 2, 3, 4}, out{};
  EXPECT_TRUE(m->write(Gpa(0x40000100), in));
  EXPECT_TRUE(m->read(Gpa(0x40000100), out));
  EXPECT_EQ(in, out);
  EXPECT_FALSE(m->write(Gpa(0x40003ffe), in));
}

TEST(GuestMemory, RoundsSizeToHostPages) {
  auto m = GuestMemory::create(Gpa(0x40000000), 1);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->size(), GuestMemory::kHostPageSize);
  EXPECT_FALSE(GuestMemory::create(Gpa(0x40000001), 4096));
}
