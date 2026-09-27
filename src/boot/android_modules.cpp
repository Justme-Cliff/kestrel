#include "boot/android_modules.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace kestrel {
namespace android_modules {
namespace {

uint16_t le16(std::span<const uint8_t> d, size_t off) {
  uint16_t v = 0;
  if (off + 2 <= d.size()) std::memcpy(&v, d.data() + off, 2);
  return v;
}
uint32_t le32(std::span<const uint8_t> d, size_t off) {
  uint32_t v = 0;
  if (off + 4 <= d.size()) std::memcpy(&v, d.data() + off, 4);
  return v;
}
uint64_t le64(std::span<const uint8_t> d, size_t off) {
  uint64_t v = 0;
  if (off + 8 <= d.size()) std::memcpy(&v, d.data() + off, 8);
  return v;
}

Result<std::vector<uint8_t>> read_range(const SuperReader& read, uint64_t off, uint64_t len) {
  if (len > (64u << 20)) return make_error("read too large");
  std::vector<uint8_t> buf(len);
  if (!read(off, buf)) return make_error(std::format("read failed at {:#x}", off));
  return buf;
}

// ---- erofs (Documentation/filesystems/erofs.rst, fs/erofs/erofs_fs.h) ----
constexpr uint32_t kErofsMagic = 0xE0F5E1E2;
constexpr uint32_t kIncompatZeroPadding = 0x1;
enum : unsigned { kFlatPlain = 0, kCompressedFull = 1, kFlatInline = 2, kCompressedCompact = 3 };
enum : unsigned { kTypePlain = 0, kTypeHead1 = 1, kTypeNonHead = 2, kTypeHead2 = 3 };
constexpr uint16_t kAdviseCompacted2B = 0x1;

struct Erofs {
  const SuperReader& read;
  uint64_t size;
  unsigned blkbits = 12;
  uint64_t meta = 0;  // byte offset of inode area
  uint16_t root = 0;
  uint64_t blk() const { return 1ull << blkbits; }
};

struct Inode {
  uint64_t loc;
  unsigned layout;
  unsigned isize, xsize;
  uint16_t mode;
  uint64_t size;
  uint32_t raw;
};

Result<Inode> read_inode(const Erofs& fs, uint64_t nid) {
  const uint64_t loc = fs.meta + nid * 32;
  auto h = read_range(fs.read, loc, 64);
  if (!h) return std::unexpected(h.error());
  const uint16_t format = le16(*h, 0);
  Inode in{};
  in.loc = loc;
  in.layout = (format >> 1) & 7;
  const bool extended = format & 1;
  in.isize = extended ? 64 : 32;
  const uint16_t xcount = le16(*h, 2);
  in.xsize = xcount ? 12 + (xcount - 1u) * 4 : 0;
  in.mode = le16(*h, 4);
  in.size = extended ? le64(*h, 8) : le32(*h, 8);
  in.raw = le32(*h, 16);
  if (in.size > (256u << 20)) return make_error("erofs: file too large");
  return in;
}

// One LZ4 physical cluster (a single block without big pclusters).
Result<std::vector<uint8_t>> read_pcluster(const Erofs& fs, uint32_t blkaddr, unsigned type, size_t out_len) {
  auto raw = read_range(fs.read, uint64_t(blkaddr) << fs.blkbits, fs.blk());
  if (!raw) return std::unexpected(raw.error());
  if (type == kTypePlain) {
    if (out_len > raw->size()) return make_error("erofs: plain cluster too short");
    raw->resize(out_len);
    return raw;
  }
  // ZERO_PADDING: compressed data is right-aligned; the first byte of an LZ4 block
  // is a token with literals, never 0.
  size_t start = 0;
  while (start < raw->size() && (*raw)[start] == 0) ++start;
  auto out = lz4_block_decompress(std::span(*raw).subspan(start), out_len);
  if (!out) return std::unexpected(out.error());
  if (out->size() < out_len) return make_error("erofs: short compressed cluster");
  return out;
}

struct Lcluster {
  unsigned type;
  unsigned clusterofs;
  uint32_t pblk;
};

unsigned decode_compacted(unsigned lobits, std::span<const uint8_t> pack, unsigned pos, unsigned* type) {
  const uint32_t v = le32(pack, pos / 8) >> (pos & 7);
  *type = (v >> lobits) & 3;
  return v & ((1u << lobits) - 1);
}

Result<std::vector<uint8_t>> read_compressed(const Erofs& fs, const Inode& in) {
  const uint64_t hdr = ((in.loc + in.isize + in.xsize + 7) & ~7ull);
  auto h = read_range(fs.read, hdr, 8);
  if (!h) return std::unexpected(h.error());
  const uint16_t advise = le16(*h, 4);
  const uint8_t algorithms = (*h)[6];
  const unsigned lclusterbits = fs.blkbits + ((*h)[7] & 7);
  if (advise & ~kAdviseCompacted2B) return make_error(std::format("erofs: unsupported compression options {:#x}", advise));
  if (algorithms != 0) return make_error("erofs: not LZ4");
  if (lclusterbits != fs.blkbits) return make_error("erofs: unsupported cluster size");
  const uint64_t lcs = 1ull << lclusterbits;
  const uint64_t total = (in.size + lcs - 1) / lcs;
  const uint64_t ebase = hdr + 8;

  // Every lcluster's index, decoded as the kernel does (fs/erofs/zmap.c).
  std::vector<Lcluster> lc(total);
  if (in.layout == kCompressedFull) {
    auto idx = read_range(fs.read, ebase, total * 8);
    if (!idx) return std::unexpected(idx.error());
    for (uint64_t i = 0; i < total; ++i) {
      lc[i] = {le16(*idx, i * 8) & 3u, le16(*idx, i * 8 + 2), le32(*idx, i * 8 + 4)};
    }
  } else {
    const unsigned initial4b = ((32 - ebase % 32) / 4) & 7;
    uint64_t compact2b = 0;
    if ((advise & kAdviseCompacted2B) && initial4b < total) compact2b = (total - initial4b) / 16 * 16;
    const uint64_t bytes = initial4b * 4 + compact2b * 2 + (total - std::min<uint64_t>(total, initial4b + compact2b)) * 4 + 64;
    auto area = read_range(fs.read, ebase, bytes);
    if (!area) return std::unexpected(area.error());
    const unsigned lobits = std::max(lclusterbits, 12u);
    for (uint64_t lcn = 0; lcn < total; ++lcn) {
      uint64_t pos = 0, n = lcn;
      unsigned shift = 2;
      if (n >= initial4b) {
        pos += initial4b * 4;
        n -= initial4b;
        if (n < compact2b) {
          shift = 1;
        } else {
          pos += compact2b * 2;
          n -= compact2b;
        }
      }
      pos += n << shift;
      const unsigned vcnt = shift == 2 ? 2 : 16;
      const unsigned packsize = vcnt << shift;
      const unsigned encodebits = ((packsize - 4) * 8) / vcnt;
      // Packs are aligned to their size on disk (absolute offsets, like the kernel).
      const uint64_t abs = ebase + pos;
      const uint64_t packstart = abs / packsize * packsize;
      if (packstart < ebase || packstart - ebase + packsize > area->size()) return make_error("erofs: index out of range");
      const auto pack = std::span<const uint8_t>(*area).subspan(packstart - ebase, packsize);
      int i = static_cast<int>((abs - packstart) >> shift);
      unsigned type;
      unsigned lo = decode_compacted(lobits, pack, encodebits * i, &type);
      if (type == kTypeNonHead) {
        lc[lcn] = {type, 0, 0};
        continue;
      }
      unsigned nblk = 1;
      while (i > 0) {
        --i;
        unsigned t;
        const unsigned l = decode_compacted(lobits, pack, encodebits * i, &t);
        if (t == kTypeNonHead) i -= static_cast<int>(l);
        if (i >= 0) ++nblk;
      }
      lc[lcn] = {type, lo, le32(pack, packsize - 4) + nblk};
    }
  }

  // Each head starts an extent that runs to the next head.
  std::vector<uint8_t> out(in.size);
  std::vector<std::pair<uint64_t, size_t>> heads;  // logical start, lcluster
  for (uint64_t i = 0; i < total; ++i) {
    if (lc[i].type == kTypeNonHead) continue;
    if (lc[i].type == kTypeHead2) return make_error("erofs: unsupported second algorithm");
    if (lc[i].clusterofs >= lcs) return make_error("erofs: bad cluster offset");
    heads.push_back({i * lcs + lc[i].clusterofs, i});
  }
  if (heads.empty() || heads[0].first != 0) return make_error("erofs: file doesn't start with a head cluster");
  for (size_t h = 0; h < heads.size(); ++h) {
    const uint64_t start = heads[h].first;
    const uint64_t end = h + 1 < heads.size() ? heads[h + 1].first : in.size;
    if (end < start || end > in.size) return make_error("erofs: bad extent");
    if (end - start > 64 * lcs) return make_error("erofs: extent too large");
    const Lcluster& l = lc[heads[h].second];
    auto seg = read_pcluster(fs, l.pblk, l.type, end - start);
    if (!seg) return std::unexpected(seg.error());
    std::memcpy(out.data() + start, seg->data(), end - start);
  }
  return out;
}

Result<std::vector<uint8_t>> read_data(const Erofs& fs, const Inode& in) {
  switch (in.layout) {
    case kFlatPlain:
      return read_range(fs.read, uint64_t(in.raw) << fs.blkbits, in.size);
    case kFlatInline: {
      const uint64_t full = in.size >> fs.blkbits << fs.blkbits;
      // A file smaller than a block is all inline; its block address is then
      // EROFS_NULL_ADDR (0xffffffff), not a real block.
      auto head = full ? read_range(fs.read, uint64_t(in.raw) << fs.blkbits, full) : Result<std::vector<uint8_t>>{};
      if (!head) return head;
      auto tail = read_range(fs.read, in.loc + in.isize + in.xsize, in.size - full);
      if (!tail) return tail;
      head->insert(head->end(), tail->begin(), tail->end());
      return head;
    }
    case kCompressedFull:
    case kCompressedCompact:
      return read_compressed(fs, in);
    default:
      return make_error(std::format("erofs: unsupported layout {}", in.layout));
  }
}

Result<uint64_t> lookup_child(const Erofs& fs, uint64_t dir_nid, std::string_view name) {
  auto dir = read_inode(fs, dir_nid);
  if (!dir) return std::unexpected(dir.error());
  if ((dir->mode & 0170000) != 0040000) return make_error("erofs: not a directory");
  auto data = read_data(fs, *dir);
  if (!data) return std::unexpected(data.error());
  const uint64_t bs = fs.blk();
  for (uint64_t b = 0; b < data->size(); b += bs) {
    const auto blk = std::span<const uint8_t>(*data).subspan(b, std::min<uint64_t>(bs, data->size() - b));
    const unsigned count = le16(blk, 8) / 12;
    if (count == 0 || count * 12 > blk.size()) return make_error("erofs: bad directory");
    for (unsigned k = 0; k < count; ++k) {
      const uint16_t off = le16(blk, k * 12 + 8);
      const size_t end = k + 1 < count ? le16(blk, (k + 1) * 12 + 8) : blk.size();
      if (off > end || end > blk.size()) return make_error("erofs: bad directory entry");
      std::string_view n(reinterpret_cast<const char*>(blk.data() + off), end - off);
      if (auto z = n.find('\0'); z != std::string_view::npos) n = n.substr(0, z);
      if (n == name) return le64(blk, k * 12);
    }
  }
  return make_error(std::format("erofs: {} not found", name));
}

}  // namespace

Result<std::vector<uint8_t>> lz4_block_decompress(std::span<const uint8_t> in, size_t max_out) {
  std::vector<uint8_t> out;
  out.reserve(max_out);
  size_t i = 0;
  auto length = [&](size_t base) -> std::optional<size_t> {
    size_t len = base;
    if (base != 15) return len;
    while (true) {
      if (i >= in.size()) return std::nullopt;
      const uint8_t b = in[i++];
      len += b;
      if (len > max_out + 16) return std::nullopt;
      if (b != 255) return len;
    }
  };
  while (i < in.size() && out.size() < max_out) {
    const uint8_t token = in[i++];
    auto lit = length(token >> 4);
    if (!lit || *lit > in.size() - i) return make_error("lz4: bad literal run");
    const size_t take = std::min(*lit, max_out - out.size());
    out.insert(out.end(), in.begin() + static_cast<long>(i), in.begin() + static_cast<long>(i + take));
    i += *lit;
    if (out.size() >= max_out || i >= in.size()) break;  // last sequence has only literals
    if (in.size() - i < 2) return make_error("lz4: truncated offset");
    const size_t offset = in[i] | (in[i + 1] << 8);
    i += 2;
    auto match = length(token & 15);
    if (!match) return make_error("lz4: bad match length");
    const size_t mlen = *match + 4;
    if (offset == 0 || offset > out.size()) return make_error("lz4: bad match offset");
    const size_t from = out.size() - offset;
    for (size_t k = 0; k < mlen && out.size() < max_out; ++k) out.push_back(out[from + k]);  // may overlap
  }
  return out;
}

Result<std::vector<uint8_t>> lz4_legacy_decompress(std::span<const uint8_t> in) {
  constexpr uint32_t kMagic = 0x184C2102;
  constexpr size_t kBlock = 8u << 20;
  if (le32(in, 0) != kMagic) return make_error("not an LZ4 legacy stream");
  std::vector<uint8_t> out;
  size_t i = 4;
  while (i + 4 <= in.size()) {
    const uint32_t n = le32(in, i);
    if (n == kMagic || n == 0) break;  // another stream follows, or padding
    i += 4;
    if (n > in.size() - i) break;  // trailing data that isn't ours
    auto block = lz4_block_decompress(in.subspan(i, n), kBlock);
    if (!block) return std::unexpected(block.error());
    out.insert(out.end(), block->begin(), block->end());
    i += n;
    if (out.size() > (512u << 20)) return make_error("ramdisk too large");
  }
  return out;
}

Result<std::vector<CpioFile>> cpio_read(std::span<const uint8_t> a) {
  std::vector<CpioFile> files;
  size_t off = 0;
  auto hex = [&](size_t at) -> std::optional<uint32_t> {
    uint32_t v = 0;
    for (size_t k = 0; k < 8; ++k) {
      const char c = static_cast<char>(a[at + k]);
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return std::nullopt;
    }
    return v;
  };
  while (off + 110 <= a.size()) {
    if (std::memcmp(a.data() + off, "070701", 6) != 0) {
      if (a[off] == 0) {  // padding between archives
        ++off;
        continue;
      }
      return make_error("cpio: bad header");
    }
    auto mode = hex(off + 6 + 8), size = hex(off + 6 + 6 * 8), namesize = hex(off + 6 + 11 * 8);
    if (!mode || !size || !namesize) return make_error("cpio: bad header field");
    const size_t name_at = off + 110;
    if (*namesize == 0 || *namesize > a.size() - name_at) return make_error("cpio: bad name");
    std::string name(reinterpret_cast<const char*>(a.data() + name_at), *namesize - 1);
    const size_t data_at = (name_at + *namesize + 3) & ~size_t(3);
    if (data_at > a.size() || *size > a.size() - data_at) return make_error("cpio: truncated");
    if (name == "TRAILER!!!") {
      off = (data_at + *size + 3) & ~size_t(3);
      continue;
    }
    while (name.starts_with("./") || name.starts_with("/")) name.erase(0, name[0] == '.' ? 2 : 1);
    const uint32_t type = *mode & 0170000;
    if (type == 0100000 || type == 0040000) {
      files.push_back({name, *mode, std::vector<uint8_t>(a.begin() + static_cast<long>(data_at),
                                                          a.begin() + static_cast<long>(data_at + *size))});
    }
    off = (data_at + *size + 3) & ~size_t(3);
  }
  return files;
}

std::vector<uint8_t> lz4_legacy_store(std::span<const uint8_t> data) {
  // Literal-only LZ4 blocks: valid LZ4, no compressor needed. The kernel reads
  // concatenated legacy streams (each starts with the magic) one after another.
  std::vector<uint8_t> out = {0x02, 0x21, 0x4C, 0x18};
  constexpr size_t kChunk = 4u << 20;
  for (size_t at = 0; at < data.size(); at += kChunk) {
    const size_t n = std::min(kChunk, data.size() - at);
    std::vector<uint8_t> block;
    block.push_back(0xF0);
    size_t rest = n - 15;
    while (rest >= 255) {
      block.push_back(255);
      rest -= 255;
    }
    block.push_back(static_cast<uint8_t>(rest));
    block.insert(block.end(), data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(at + n));
    const uint32_t size = static_cast<uint32_t>(block.size());
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(size >> (8 * k)));
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

std::vector<uint8_t> cpio_write(const std::vector<CpioFile>& files) {
  std::vector<uint8_t> out;
  uint32_t ino = 0x4b53000;  // clear of the real archives' inode numbers
  auto entry = [&](const std::string& name, uint32_t mode, std::span<const uint8_t> data) {
    const std::string h = std::format("070701{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}{:08x}",
                                      ino++, mode, 0, 0, 1, 0, data.size(), 0, 0, 0, 0, name.size() + 1, 0);
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    while (out.size() % 4) out.push_back(0);
    out.insert(out.end(), data.begin(), data.end());
    while (out.size() % 4) out.push_back(0);
  };
  for (const auto& f : files) entry(f.name, f.mode, f.data);
  entry("TRAILER!!!", 0, {});
  while (out.size() % 512) out.push_back(0);
  return out;
}

Result<std::vector<uint8_t>> erofs_read_file(const SuperReader& read, uint64_t image_size, const std::string& path) {
  auto sb = read_range(read, 1024, 128);
  if (!sb) return std::unexpected(sb.error());
  if (le32(*sb, 0) != kErofsMagic) return make_error("not an erofs image");
  Erofs fs{read, image_size};
  fs.blkbits = (*sb)[12];
  if (fs.blkbits < 9 || fs.blkbits > 16) return make_error("erofs: bad block size");
  fs.root = le16(*sb, 14);
  fs.meta = uint64_t(le32(*sb, 40)) << fs.blkbits;
  const uint32_t incompat = le32(*sb, 0x50);
  if (incompat & ~kIncompatZeroPadding) return make_error(std::format("erofs: unsupported features {:#x}", incompat));
  uint64_t nid = fs.root;
  size_t start = 0;
  while (start < path.size()) {
    size_t slash = path.find('/', start);
    if (slash == std::string::npos) slash = path.size();
    if (slash > start) {
      auto child = lookup_child(fs, nid, std::string_view(path).substr(start, slash - start));
      if (!child) return std::unexpected(child.error());
      nid = *child;
    }
    start = slash + 1;
  }
  auto in = read_inode(fs, nid);
  if (!in) return std::unexpected(in.error());
  if ((in->mode & 0170000) != 0100000) return make_error("erofs: not a file");
  return read_data(fs, *in);
}

Result<Extent> super_partition(const SuperReader& read, const std::string& name) {
  // system/core/fs_mgr/liblp/include/liblp/metadata_format.h
  constexpr uint64_t kGeometry = 4096, kMetadata = 4096 + 2 * 4096;
  auto geo = read_range(read, kGeometry, 64);
  if (!geo) return std::unexpected(geo.error());
  if (le32(*geo, 0) != 0x616c4467) return make_error("super: no LP geometry");
  auto hdr = read_range(read, kMetadata, 256);
  if (!hdr) return std::unexpected(hdr.error());
  if (le32(*hdr, 0) != 0x414C5030) return make_error("super: no LP metadata");
  const uint32_t header_size = le32(*hdr, 8), tables_size = le32(*hdr, 44);
  if (header_size > 256 || tables_size > (1u << 20)) return make_error("super: bad metadata size");
  auto tables = read_range(read, kMetadata + header_size, tables_size);
  if (!tables) return std::unexpected(tables.error());
  const size_t desc = 80;  // after magic, versions, sizes and both checksums
  const uint32_t p_off = le32(*hdr, desc), p_num = le32(*hdr, desc + 4), p_size = le32(*hdr, desc + 8);
  const uint32_t e_off = le32(*hdr, desc + 12), e_num = le32(*hdr, desc + 16), e_size = le32(*hdr, desc + 20);
  if (p_size < 52 || e_size < 24 || uint64_t(p_off) + uint64_t(p_num) * p_size > tables->size() ||
      uint64_t(e_off) + uint64_t(e_num) * e_size > tables->size()) {
    return make_error("super: bad tables");
  }
  for (uint32_t i = 0; i < p_num; ++i) {
    const size_t p = p_off + size_t(i) * p_size;
    const std::string n(reinterpret_cast<const char*>(tables->data() + p), strnlen(reinterpret_cast<const char*>(tables->data() + p), 36));
    if (n != name) continue;
    const uint32_t first = le32(*tables, p + 40), count = le32(*tables, p + 44);
    if (count != 1 || first >= e_num) return make_error("super: " + name + " isn't one extent");
    const size_t e = e_off + size_t(first) * e_size;
    if (le32(*tables, e + 8) != 0) return make_error("super: " + name + " isn't linear");
    return Extent{le64(*tables, e + 12) * 512, le64(*tables, e) * 512};
  }
  return make_error("super: no partition " + name);
}

}  // namespace android_modules

Result<std::vector<uint8_t>> build_extra_modules_cpio(const SuperReader& super, std::span<const uint8_t> vendor_ramdisk,
                                                      const std::vector<std::string>& modules) {
  using namespace android_modules;
  auto part = super_partition(super, "system_dlkm_a");
  if (!part) return std::unexpected(part.error());
  const Extent ext = *part;
  const SuperReader in_part = [&](uint64_t off, std::span<uint8_t> out) {
    if (off > ext.size || out.size() > ext.size - off) return false;
    return super(ext.offset + off, out);
  };
  auto raw = lz4_legacy_decompress(vendor_ramdisk);
  if (!raw) return std::unexpected(raw.error());
  auto files = cpio_read(*raw);
  if (!files) return std::unexpected(files.error());
  auto find = [&](const std::string& n) -> const CpioFile* {
    const CpioFile* last = nullptr;
    for (const auto& f : *files) if (f.name == n) last = &f;
    return last;
  };
  const CpioFile* load = find("lib/modules/modules.load");
  const CpioFile* dep = find("lib/modules/modules.dep");
  if (!load || !dep) return make_error("vendor ramdisk has no module lists");
  auto system_dep = erofs_read_file(in_part, ext.size, "/lib/modules/modules.dep");
  if (!system_dep) return std::unexpected(system_dep.error());
  const std::string sys_dep(system_dep->begin(), system_dep->end());

  std::string load_text(load->data.begin(), load->data.end());
  std::string dep_text(dep->data.begin(), dep->data.end());
  if (!load_text.empty() && load_text.back() != '\n') load_text += '\n';
  if (!dep_text.empty() && dep_text.back() != '\n') dep_text += '\n';
  std::vector<CpioFile> add;
  for (const auto& m : modules) {
    if (m.find('/') != std::string::npos || !m.ends_with(".ko")) return make_error("bad module name " + m);
    // Only modules with no dependencies: their paths in the ramdisk would differ.
    const std::string key = "/system/lib/modules/" + m + ":";
    const size_t at = sys_dep.find(key);
    if (at == std::string::npos) return make_error(m + " isn't in system_dlkm");
    const size_t eol = sys_dep.find('\n', at);
    if (sys_dep.substr(at + key.size(), eol - at - key.size()).find_first_not_of(" \t\r") != std::string::npos) {
      return make_error(m + " has dependencies");
    }
    if (load_text.find(m + "\n") != std::string::npos) continue;  // already loaded
    auto ko = erofs_read_file(in_part, ext.size, "/lib/modules/" + m);
    if (!ko) return std::unexpected(ko.error());
    if (ko->size() < 4 || std::memcmp(ko->data(), "\x7f" "ELF", 4) != 0) return make_error(m + " isn't an ELF module");
    add.push_back({"lib/modules/" + m, 0100644, std::move(*ko)});
    load_text += m + "\n";
    dep_text += "/lib/modules/" + m + ":\n";
  }
  if (add.empty()) return std::vector<uint8_t>{};
  add.push_back({"lib/modules/modules.load", 0100644, std::vector<uint8_t>(load_text.begin(), load_text.end())});
  add.push_back({"lib/modules/modules.dep", 0100644, std::vector<uint8_t>(dep_text.begin(), dep_text.end())});
  return lz4_legacy_store(cpio_write(add));
}

}  // namespace kestrel
