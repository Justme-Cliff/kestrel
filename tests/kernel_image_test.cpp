#include "boot/kernel_image.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>

using namespace kestrel;

namespace {
std::vector<uint8_t> fake_image(uint64_t text_offset, uint64_t image_size, size_t file_size) {
  std::vector<uint8_t> img(file_size, 0);
  std::memcpy(img.data() + 8, &text_offset, 8);
  std::memcpy(img.data() + 16, &image_size, 8);
  std::memcpy(img.data() + 0x38, "ARM\x64", 4);
  return img;
}

std::vector<uint8_t> gzip(const std::vector<uint8_t>& in) {
  z_stream zs{};
  deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  std::vector<uint8_t> out(deflateBound(&zs, in.size()) + 32);
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}
}  // namespace

TEST(KernelImage, ParsesHeader) {
  auto info = parse_arm64_image(fake_image(0, 0x200000, 4096));
  ASSERT_TRUE(info);
  EXPECT_EQ(info->text_offset, 0u);
  EXPECT_EQ(info->image_size, 0x200000u);
}

TEST(KernelImage, RejectsBadMagic) {
  auto img = fake_image(0, 0x200000, 4096);
  img[0x38] = 'X';
  EXPECT_FALSE(parse_arm64_image(img));
  EXPECT_FALSE(parse_arm64_image(std::vector<uint8_t>(10)));
}

TEST(KernelImage, UnpacksGzip) {
  auto img = fake_image(0, 0x200000, 100000);
  auto out = unpack_kernel(gzip(img));
  ASSERT_TRUE(out);
  EXPECT_EQ(*out, img);
}

TEST(KernelImage, UnpacksZboot) {
  auto img = fake_image(0, 0x200000, 50000);
  auto payload = gzip(img);
  std::vector<uint8_t> zboot(0x100, 0);
  zboot[0] = 'M';
  zboot[1] = 'Z';
  std::memcpy(zboot.data() + 4, "zimg", 4);
  uint32_t off = 0x100, size = static_cast<uint32_t>(payload.size());
  std::memcpy(zboot.data() + 8, &off, 4);
  std::memcpy(zboot.data() + 12, &size, 4);
  std::memcpy(zboot.data() + 0x18, "gzip", 4);
  zboot.insert(zboot.end(), payload.begin(), payload.end());
  auto out = unpack_kernel(zboot);
  ASSERT_TRUE(out);
  EXPECT_EQ(*out, img);
}

TEST(KernelImage, PassesRawImageThrough) {
  auto img = fake_image(0, 0x200000, 4096);
  auto out = unpack_kernel(img);
  ASSERT_TRUE(out);
  EXPECT_EQ(*out, img);
}
