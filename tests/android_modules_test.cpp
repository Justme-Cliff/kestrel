#include "boot/android_modules.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace kestrel;
using namespace kestrel::android_modules;

TEST(AndroidModules, Lz4StoreRoundTrips) {
  std::vector<uint8_t> data(70000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7);
  auto out = lz4_legacy_decompress(lz4_legacy_store(data));
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(*out, data);
}

TEST(AndroidModules, Lz4DecodesOverlappingMatches) {
  // Token 0x24: 2 literals "ab", then a match at offset 2 of length 4+4 = 8 that
  // overlaps what it copies; a final empty literal run ends the block.
  const std::vector<uint8_t> block = {0x24, 'a', 'b', 0x02, 0x00, 0x00};
  auto out = lz4_block_decompress(block, 100);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(std::string(out->begin(), out->end()), "ababababab");
}

TEST(AndroidModules, Lz4RejectsBadOffsets) {
  const std::vector<uint8_t> block = {0x10, 'a', 0x05, 0x00};  // offset 5 > output size
  EXPECT_FALSE(lz4_block_decompress(block, 100));
  const std::vector<uint8_t> runaway = {0xF0, 255, 255, 255};  // literal run past the input
  EXPECT_FALSE(lz4_block_decompress(runaway, 100));
}

TEST(AndroidModules, CpioRoundTrips) {
  std::vector<CpioFile> files = {{"lib/modules/a.ko", 0100644, {1, 2, 3}}, {"lib/modules/modules.load", 0100644, {'a', '\n'}}};
  auto back = cpio_read(cpio_write(files));
  ASSERT_TRUE(back) << back.error().message;
  ASSERT_EQ(back->size(), 2u);
  EXPECT_EQ((*back)[0].name, "lib/modules/a.ko");
  EXPECT_EQ((*back)[0].data, files[0].data);
  EXPECT_EQ((*back)[1].data, files[1].data);
}

TEST(AndroidModules, CpioRejectsTruncation) {
  auto a = cpio_write({{"x", 0100644, std::vector<uint8_t>(100, 7)}});
  a.resize(150);
  EXPECT_FALSE(cpio_read(a));
}

// The real Cuttlefish image, when this Mac has it (skipped on CI).
TEST(AndroidModules, ExtractsBalloonFromRealImage) {
  const char* home = std::getenv("HOME");
  const std::filesystem::path dir = std::filesystem::path(home ? home : "") / "Library/Application Support/Kestrel/android";
  if (!std::filesystem::exists(dir / "instance/super.raw")) GTEST_SKIP() << "no Android image";
  std::ifstream super(dir / "instance/super.raw", std::ios::binary);
  const SuperReader read = [&](uint64_t off, std::span<uint8_t> out) {
    super.clear();
    super.seekg(static_cast<std::streamoff>(off));
    super.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return bool(super);
  };
  auto part = super_partition(read, "system_dlkm_a");
  ASSERT_TRUE(part) << part.error().message;
  const SuperReader in_part = [&](uint64_t off, std::span<uint8_t> out) {
    if (off > part->size || out.size() > part->size - off) return false;  // like the app's reader
    return read(part->offset + off, out);
  };
  auto ko = erofs_read_file(in_part, part->size, "/lib/modules/virtio_balloon.ko");
  ASSERT_TRUE(ko) << ko.error().message;
  ASSERT_GT(ko->size(), 1000u);
  EXPECT_EQ(std::string(ko->begin(), ko->begin() + 4), "\x7f" "ELF");
  const std::string text(ko->begin(), ko->end());
  EXPECT_NE(text.find("name=virtio_balloon"), std::string::npos);
  EXPECT_NE(text.find("vermagic="), std::string::npos);
  auto dep = erofs_read_file(in_part, part->size, "/lib/modules/modules.dep");  // inline directories on the way
  ASSERT_TRUE(dep) << dep.error().message;
  EXPECT_NE(std::string(dep->begin(), dep->end()).find("/system/lib/modules/virtio_balloon.ko:"), std::string::npos);
  if (const char* ref = std::getenv("KESTREL_REF_KO")) {  // byte-exact check against erofs-utils
    std::ifstream f(ref, std::ios::binary);
    std::vector<uint8_t> want((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(*ko, want);
  }
}
