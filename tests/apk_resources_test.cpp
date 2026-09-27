#include "android/apk_resources.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

using namespace kestrel;

TEST(ApkResources, RejectsGarbage) {
  std::vector<uint8_t> empty;
  EXPECT_FALSE(parse_manifest(empty));
  EXPECT_FALSE(ResourceTable::parse(empty));
  EXPECT_FALSE(parse_adaptive_icon(empty));
  // A chunk header claiming more bytes than there are.
  std::vector<uint8_t> lying = {0x03, 0x00, 0x08, 0x00, 0xff, 0xff, 0x00, 0x00};
  EXPECT_FALSE(parse_manifest(lying));
  std::vector<uint8_t> table = {0x02, 0x00, 0x0c, 0x00, 0xff, 0xff, 0xff, 0x7f, 0, 0, 0, 0};
  EXPECT_FALSE(ResourceTable::parse(table));
}

TEST(ApkResources, SurvivesRandomBytes) {
  // Random data behind valid-looking headers must never crash or hang.
  std::mt19937 rng(42);
  for (int round = 0; round < 2000; ++round) {
    std::vector<uint8_t> d(8 + rng() % 600);
    for (auto& b : d) b = static_cast<uint8_t>(rng());
    const uint16_t type = round % 2 ? 0x0003 : 0x0002;
    d[0] = static_cast<uint8_t>(type), d[1] = 0;
    d[2] = 8, d[3] = 0;
    d[4] = static_cast<uint8_t>(d.size()), d[5] = static_cast<uint8_t>(d.size() >> 8), d[6] = 0, d[7] = 0;
    (void)parse_manifest(d);
    (void)parse_adaptive_icon(d);
    if (auto t = ResourceTable::parse(d)) (void)t->resolve(0x7f010000);
  }
  SUCCEED();
}
