#include "devices/virtio/edid.h"

#include <gtest/gtest.h>

using namespace kestrel;

TEST(Edid, HeaderChecksumAndMode) {
  auto e = make_edid(1280, 720, 120);
  EXPECT_EQ(e[0], 0x00);
  EXPECT_EQ(e[1], 0xff);
  EXPECT_EQ(e[7], 0x00);
  uint8_t sum = 0;
  for (auto b : e) sum = static_cast<uint8_t>(sum + b);
  EXPECT_EQ(sum, 0);
  // Detailed timing decodes back to the requested mode and refresh rate.
  const uint8_t* d = e.data() + 54;
  const uint32_t clock = (d[0] | d[1] << 8) * 10000u;
  const uint32_t w = d[2] | (d[4] >> 4) << 8, hb = d[3] | (d[4] & 0xf) << 8;
  const uint32_t h = d[5] | (d[7] >> 4) << 8, vb = d[6] | (d[7] & 0xf) << 8;
  EXPECT_EQ(w, 1280u);
  EXPECT_EQ(h, 720u);
  const double hz = double(clock) / ((w + hb) * (h + vb));
  EXPECT_NEAR(hz, 120.0, 0.1);
}

TEST(Edid, ThirtyHertz) {
  auto e = make_edid(1920, 1080, 30);
  const uint8_t* d = e.data() + 54;
  const uint32_t clock = (d[0] | d[1] << 8) * 10000u;
  const uint32_t w = d[2] | (d[4] >> 4) << 8, hb = d[3] | (d[4] & 0xf) << 8;
  const uint32_t h = d[5] | (d[7] >> 4) << 8, vb = d[6] | (d[7] & 0xf) << 8;
  EXPECT_NEAR(double(clock) / ((w + hb) * (h + vb)), 30.0, 0.1);
}
