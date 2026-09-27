#include "boot/android_boot.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace kestrel;

TEST(Bootconfig, ParsesVendorSection) {
  auto m = parse_bootconfig("androidboot.hardware=cutf_cvm\nkernel.foo = \"16384\"\n\n");
  EXPECT_EQ(m["androidboot.hardware"], "cutf_cvm");
  EXPECT_EQ(m["kernel.foo"], "16384");
}

TEST(Bootconfig, AppendsTrailerTheKernelAccepts) {
  std::vector<uint8_t> initrd = {1, 2, 3};
  append_bootconfig(initrd, {{"androidboot.slot_suffix", "_a"}});
  const std::string magic = "#BOOTCONFIG\n";
  ASSERT_GE(initrd.size(), 3 + 8 + magic.size());
  EXPECT_EQ(std::string(initrd.end() - 12, initrd.end()), magic);
  uint32_t size, checksum;
  std::memcpy(&size, &*(initrd.end() - 20), 4);
  std::memcpy(&checksum, &*(initrd.end() - 16), 4);
  EXPECT_EQ(size % 4, 0u);
  EXPECT_EQ(initrd.size(), 3 + size + 8 + magic.size());
  const std::string data(initrd.begin() + 3, initrd.begin() + 3 + size);
  EXPECT_EQ(data.substr(0, data.find('\0')), "androidboot.slot_suffix = \"_a\"\n");
  uint32_t sum = 0;
  for (unsigned char c : data) sum += c;
  EXPECT_EQ(sum, checksum);
}
