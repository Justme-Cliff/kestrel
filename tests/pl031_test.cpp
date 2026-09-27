#include "devices/platform/pl031.h"

#include <gtest/gtest.h>

using namespace kestrel;

TEST(Pl031, ReportsHostTimeAndAcceptsLoad) {
  uint32_t host = 1'700'000'000;
  bool irq = false;
  Pl031 rtc([&] { return host; }, [&](bool l) { irq = l; });
  EXPECT_EQ(rtc.mmio_read(0x00, 4), host);
  rtc.mmio_write(0x08, 4, 1000);  // guest sets the clock
  host += 5;
  EXPECT_EQ(rtc.mmio_read(0x00, 4), 1005u);
  EXPECT_FALSE(irq);
}

TEST(Pl031, IdentifiesAsPl031) {
  Pl031 rtc([] { return 0u; }, [](bool) {});
  EXPECT_EQ(rtc.mmio_read(0xfe0, 4), 0x31u);
  EXPECT_EQ(rtc.mmio_read(0xff0, 4), 0x0du);
}
