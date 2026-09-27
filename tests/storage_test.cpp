#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "storage/android_sparse.h"
#include "storage/composite_disk.h"

using namespace kestrel;
namespace fs = std::filesystem;

namespace {

class MemFile : public BlockFile {
 public:
  MemFile(size_t size, uint8_t fill, bool ro) : data(size, fill), ro_(ro) {}
  uint64_t size() const override { return data.size(); }
  bool read_only() const override { return ro_; }
  bool read_at(uint64_t off, std::span<uint8_t> out) override {
    if (off + out.size() > data.size()) return false;
    std::memcpy(out.data(), data.data() + off, out.size());
    return true;
  }
  bool write_at(uint64_t off, std::span<const uint8_t> in) override {
    if (ro_ || off + in.size() > data.size()) return false;
    std::memcpy(data.data() + off, in.data(), in.size());
    return true;
  }
  bool flush() override { return true; }
  std::vector<uint8_t> data;

 private:
  bool ro_;
};

fs::path temp_path(const char* name) { return fs::temp_directory_path() / name; }

template <typename T>
void put(std::ofstream& out, T v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

}  // namespace

TEST(AndroidSparse, ExpandsRawFillAndDontCare) {
  const auto in = temp_path("kestrel_sparse_in.img"), out = temp_path("kestrel_sparse_out.img");
  {
    std::ofstream f(in, std::ios::binary);
    put<uint32_t>(f, 0xed26ff3a);
    put<uint16_t>(f, 1);
    put<uint16_t>(f, 0);
    put<uint16_t>(f, 28);
    put<uint16_t>(f, 12);
    put<uint32_t>(f, 4096);  // block size
    put<uint32_t>(f, 4);     // total blocks
    put<uint32_t>(f, 3);     // chunks
    put<uint32_t>(f, 0);
    // raw: 1 block of 0xAB
    put<uint16_t>(f, 0xcac1); put<uint16_t>(f, 0); put<uint32_t>(f, 1); put<uint32_t>(f, 12 + 4096);
    std::vector<char> raw(4096, char(0xab));
    f.write(raw.data(), raw.size());
    // don't care: 2 blocks
    put<uint16_t>(f, 0xcac3); put<uint16_t>(f, 0); put<uint32_t>(f, 2); put<uint32_t>(f, 12);
    // fill: 1 block of 0x11223344
    put<uint16_t>(f, 0xcac2); put<uint16_t>(f, 0); put<uint32_t>(f, 1); put<uint32_t>(f, 16);
    put<uint32_t>(f, 0x11223344);
  }
  ASSERT_TRUE(is_android_sparse(in.string()));
  ASSERT_TRUE(unsparse_image(in.string(), out.string()));
  ASSERT_EQ(fs::file_size(out), 4u * 4096);
  std::ifstream r(out, std::ios::binary);
  std::vector<uint8_t> d(4 * 4096);
  r.read(reinterpret_cast<char*>(d.data()), d.size());
  EXPECT_EQ(d[0], 0xab);
  EXPECT_EQ(d[4095], 0xab);
  EXPECT_EQ(d[4096], 0);
  EXPECT_EQ(d[3 * 4096], 0x44);
  EXPECT_EQ(d[3 * 4096 + 3], 0x11);
  fs::remove(in);
  fs::remove(out);
}

TEST(CompositeDisk, BuildsValidGptAndRoutesIo) {
  std::vector<CompositeDisk::Partition> parts;
  auto* boot = new MemFile(8192, 0xb0, /*ro=*/true);
  auto* data = new MemFile(4096, 0, /*ro=*/false);
  parts.push_back({"boot_a", std::unique_ptr<BlockFile>(boot)});
  parts.push_back({"userdata", std::unique_ptr<BlockFile>(data)});
  auto disk = CompositeDisk::create(std::move(parts));
  ASSERT_TRUE(disk);
  auto& d = **disk;

  std::vector<uint8_t> sector(512);
  ASSERT_TRUE(d.read_at(0, sector));
  EXPECT_EQ(sector[510], 0x55);
  EXPECT_EQ(sector[450], 0xee);
  ASSERT_TRUE(d.read_at(512, sector));
  EXPECT_EQ(std::memcmp(sector.data(), "EFI PART", 8), 0);
  uint32_t crc_stored;
  std::memcpy(&crc_stored, &sector[16], 4);
  std::memset(&sector[16], 0, 4);
  EXPECT_EQ(crc32(0, sector.data(), 92), crc_stored);

  // Entry 1 is "userdata"; check its name and that its first LBA maps to the file.
  std::vector<uint8_t> entries(128 * 128);
  ASSERT_TRUE(d.read_at(1024, entries));
  EXPECT_EQ(entries[128 + 56], 'u');
  uint64_t first_lba;
  std::memcpy(&first_lba, &entries[128 + 32], 8);
  std::vector<uint8_t> msg = {'h', 'i'};
  ASSERT_TRUE(d.write_at(first_lba * 512, msg));
  EXPECT_EQ(data->data[0], 'h');

  // Read-only partitions and GPT sectors refuse writes; boot reads its bytes.
  uint64_t boot_lba;
  std::memcpy(&boot_lba, &entries[32], 8);
  EXPECT_FALSE(d.write_at(boot_lba * 512, msg));
  EXPECT_FALSE(d.write_at(0, msg));
  ASSERT_TRUE(d.read_at(boot_lba * 512, sector));
  EXPECT_EQ(sector[0], 0xb0);

  // Backup header in the last sector.
  ASSERT_TRUE(d.read_at(d.size() - 512, sector));
  EXPECT_EQ(std::memcmp(sector.data(), "EFI PART", 8), 0);
}
