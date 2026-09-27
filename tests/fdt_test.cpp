#include "boot/fdt_writer.h"

#include <gtest/gtest.h>

#include <cstring>

#include "boot/virt_dtb.h"

using namespace kestrel;

namespace {
uint32_t be32(const std::vector<uint8_t>& d, size_t off) {
  return (uint32_t(d[off]) << 24) | (uint32_t(d[off + 1]) << 16) | (uint32_t(d[off + 2]) << 8) |
         d[off + 3];
}
bool contains(const std::vector<uint8_t>& d, std::string_view s) {
  return std::search(d.begin(), d.end(), s.begin(), s.end()) != d.end();
}
}  // namespace

TEST(FdtWriter, HeaderIsConsistent) {
  FdtWriter w;
  w.begin_node("");
  w.prop_u32("#address-cells", 2);
  w.begin_node("chosen");
  w.prop_string("bootargs", "console=ttyAMA0");
  w.end_node();
  w.end_node();
  auto blob = w.finish();
  ASSERT_TRUE(blob);
  EXPECT_EQ(be32(*blob, 0), 0xd00dfeedu);
  EXPECT_EQ(be32(*blob, 4), blob->size());
  EXPECT_EQ(be32(*blob, 20), 17u);
  const uint32_t off_struct = be32(*blob, 8), size_struct = be32(*blob, 36);
  EXPECT_EQ(be32(*blob, 12), off_struct + size_struct);  // strings follow struct
  EXPECT_EQ(be32(*blob, off_struct), 1u);                // FDT_BEGIN_NODE
  EXPECT_EQ(be32(*blob, off_struct + size_struct - 4), 9u);  // FDT_END
  EXPECT_TRUE(contains(*blob, "console=ttyAMA0"));
}

TEST(FdtWriter, DeduplicatesPropertyNames) {
  FdtWriter w;
  w.begin_node("");
  w.prop_u32("reg", 1);
  w.begin_node("a");
  w.prop_u32("reg", 2);
  w.end_node();
  w.end_node();
  auto blob = w.finish();
  ASSERT_TRUE(blob);
  EXPECT_EQ(be32(*blob, 32), 4u);  // size_dt_strings == "reg\0"
}

TEST(FdtWriter, FailsOnUnclosedNode) {
  FdtWriter w;
  w.begin_node("");
  EXPECT_FALSE(w.finish());
}

TEST(VirtDtb, DescribesBoard) {
  VirtBoardConfig cfg{.ram_base = 0x40000000, .ram_size = 1ull << 30, .cpus = 1,
                      .gic_dist_base = 0x8000000, .gic_dist_size = 0x10000,
                      .gic_redist_base = 0x80a0000, .gic_redist_size = 0x20000,
                      .cmdline = "console=ttyAMA0", .initrd = {{0x48000000, 0x48100000}}};
  auto blob = build_virt_dtb(cfg);
  ASSERT_TRUE(blob);
  for (auto s : {"arm,gic-v3", "arm,pl011", "arm,psci-1.0", "memory@40000000", "linux,initrd-start",
                 "/pl011@9000000", "arm,armv8-timer"}) {
    EXPECT_TRUE(contains(*blob, s)) << s;
  }
}
