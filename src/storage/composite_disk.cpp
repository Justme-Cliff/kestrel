#include "storage/composite_disk.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#include "core/gpa.h"

namespace kestrel {
namespace {

constexpr uint32_t kEntries = 128;
constexpr uint32_t kEntrySize = 128;
constexpr uint64_t kEntrySectors = kEntries * kEntrySize / CompositeDisk::kSector;  // 32
constexpr uint64_t kFirstUsable = 2 + kEntrySectors;                               // LBA 34

// Linux filesystem data: 0FC63DAF-8483-4772-8E79-3D69D8477DE4, in GPT's mixed-endian form.
constexpr std::array<uint8_t, 16> kTypeLinuxData = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                                    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

void put_le(std::vector<uint8_t>& b, size_t off, uint64_t v, unsigned n) {
  for (unsigned i = 0; i < n; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

// Stable per-name GUID so the disk looks identical on every boot.
std::array<uint8_t, 16> name_guid(std::string_view name, uint8_t salt) {
  std::array<uint8_t, 16> g{};
  uint64_t h1 = 1469598103934665603ull ^ salt, h2 = 0x9e3779b97f4a7c15ull ^ salt;
  for (char c : name) {
    h1 = (h1 ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    h2 = (h2 ^ static_cast<uint8_t>(c)) * 0x100000001b3ull + 7;
  }
  std::memcpy(g.data(), &h1, 8);
  std::memcpy(g.data() + 8, &h2, 8);
  g[7] = (g[7] & 0x0f) | 0x40;  // version 4
  g[8] = (g[8] & 0x3f) | 0x80;  // RFC 4122 variant
  return g;
}

std::vector<uint8_t> gpt_header(uint64_t current, uint64_t backup, uint64_t last_usable,
                                uint64_t entries_lba, uint32_t entries_crc) {
  std::vector<uint8_t> h(CompositeDisk::kSector, 0);
  std::memcpy(h.data(), "EFI PART", 8);
  put_le(h, 8, 0x00010000, 4);  // revision 1.0
  put_le(h, 12, 92, 4);         // header size
  put_le(h, 24, current, 8);
  put_le(h, 32, backup, 8);
  put_le(h, 40, kFirstUsable, 8);
  put_le(h, 48, last_usable, 8);
  auto disk_guid = name_guid("kestrel-disk", 0);
  std::memcpy(&h[56], disk_guid.data(), 16);
  put_le(h, 72, entries_lba, 8);
  put_le(h, 80, kEntries, 4);
  put_le(h, 84, kEntrySize, 4);
  put_le(h, 88, entries_crc, 4);
  put_le(h, 16, crc32(0, h.data(), 92), 4);
  return h;
}

}  // namespace

Result<std::unique_ptr<CompositeDisk>> CompositeDisk::create(std::vector<Partition> partitions) {
  if (partitions.empty() || partitions.size() > kEntries) {
    return make_error("composite disk needs 1-128 partitions");
  }
  auto disk = std::unique_ptr<CompositeDisk>(new CompositeDisk());

  std::vector<uint8_t> entries(kEntries * kEntrySize, 0);
  uint64_t cursor = kAlign;
  for (size_t i = 0; i < partitions.size(); ++i) {
    const auto& p = partitions[i];
    if (p.name.empty() || p.name.size() > 36) return make_error("bad partition name " + p.name);
    // Exact size (rounded to a sector): AVB reads footers at the very end of a partition.
    const uint64_t size = align_up(p.file->size(), kSector);
    if (size == 0) return make_error("empty partition " + p.name);
    const uint64_t first = cursor / kSector, last = (cursor + size) / kSector - 1;
    auto e = std::span<uint8_t>(entries).subspan(i * kEntrySize, kEntrySize);
    std::copy(kTypeLinuxData.begin(), kTypeLinuxData.end(), e.begin());
    auto unique = name_guid(p.name, 1);
    std::copy(unique.begin(), unique.end(), e.begin() + 16);
    std::vector<uint8_t> tmp(kEntrySize, 0);
    put_le(tmp, 32, first, 8);
    put_le(tmp, 40, last, 8);
    for (size_t k = 0; k < p.name.size(); ++k) tmp[56 + 2 * k] = static_cast<uint8_t>(p.name[k]);
    std::copy(tmp.begin() + 32, tmp.end(), e.begin() + 32);
    disk->extents_.push_back({cursor, size, p.file.get(), {}});
    cursor = align_up(cursor + size, kAlign);
  }
  // Room for the backup GPT (entries + header) after the last partition.
  const uint64_t total_sectors = cursor / kSector + kEntrySectors + 1;
  const uint64_t last_lba = total_sectors - 1;
  const uint64_t last_usable = last_lba - kEntrySectors - 1;
  disk->size_ = total_sectors * kSector;
  const uint32_t entries_crc = crc32(0, entries.data(), static_cast<uInt>(entries.size()));

  // Primary: protective MBR, header, entries.
  std::vector<uint8_t> primary(kFirstUsable * kSector, 0);
  primary[446 + 4] = 0xee;  // protective partition type
  put_le(primary, 446 + 1, 0x000200, 3);
  put_le(primary, 446 + 5, 0xffffff, 3);
  put_le(primary, 446 + 8, 1, 4);
  put_le(primary, 446 + 12, std::min<uint64_t>(last_lba, 0xffffffff), 4);
  primary[510] = 0x55;
  primary[511] = 0xaa;
  auto hdr = gpt_header(1, last_lba, last_usable, 2, entries_crc);
  std::copy(hdr.begin(), hdr.end(), primary.begin() + kSector);
  std::copy(entries.begin(), entries.end(), primary.begin() + 2 * kSector);

  // Backup: entries then header in the last sectors.
  std::vector<uint8_t> backup(entries);
  auto bhdr = gpt_header(last_lba, 1, last_usable, last_lba - kEntrySectors, entries_crc);
  backup.insert(backup.end(), bhdr.begin(), bhdr.end());

  disk->extents_.insert(disk->extents_.begin(), {0, primary.size(), nullptr, std::move(primary)});
  disk->extents_.push_back({(last_lba - kEntrySectors) * kSector, backup.size(), nullptr, std::move(backup)});
  disk->partitions_ = std::move(partitions);
  return disk;
}

bool CompositeDisk::read_at(uint64_t offset, std::span<uint8_t> out) {
  if (offset > size_ || out.size() > size_ - offset) return false;
  std::ranges::fill(out, 0);
  for (const auto& e : extents_) {
    const uint64_t lo = std::max(offset, e.start), hi = std::min(offset + out.size(), e.start + e.size);
    if (lo >= hi) continue;
    auto dst = out.subspan(lo - offset, hi - lo);
    const uint64_t rel = lo - e.start;
    if (!e.file) {
      std::memcpy(dst.data(), e.synthetic.data() + rel, dst.size());
      continue;
    }
    // Bytes past the backing file's end (sector rounding) read as zeros.
    const uint64_t file_size = e.file->size();
    if (rel < file_size) {
      const uint64_t n = std::min<uint64_t>(dst.size(), file_size - rel);
      if (!e.file->read_at(rel, dst.first(n))) return false;
    }
  }
  return true;
}

bool CompositeDisk::write_at(uint64_t offset, std::span<const uint8_t> in) {
  if (offset > size_ || in.size() > size_ - offset) return false;
  uint64_t covered = 0;
  for (const auto& e : extents_) {
    const uint64_t lo = std::max(offset, e.start), hi = std::min(offset + in.size(), e.start + e.size);
    if (lo >= hi) continue;
    if (!e.file || e.file->read_only()) return false;  // GPT and read-only images are immutable
    const uint64_t rel = lo - e.start;
    if (rel + (hi - lo) > e.file->size()) return false;
    if (!e.file->write_at(rel, in.subspan(lo - offset, hi - lo))) return false;
    covered += hi - lo;
  }
  return covered == in.size();  // writes into alignment gaps fail
}

bool CompositeDisk::flush() {
  bool ok = true;
  for (auto& p : partitions_) ok = p.file->flush() && ok;
  return ok;
}

}  // namespace kestrel
