#include "boot/fdt_writer.h"

#include <format>

namespace kestrel {
namespace {

constexpr uint32_t kMagic = 0xd00dfeed;
constexpr uint32_t kBeginNode = 1;
constexpr uint32_t kEndNode = 2;
constexpr uint32_t kProp = 3;
constexpr uint32_t kEnd = 9;
constexpr uint32_t kHeaderSize = 40;
constexpr uint32_t kReserveMapSize = 16;  // just the terminating empty entry

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

}  // namespace

FdtWriter::FdtWriter() = default;

void FdtWriter::token(uint32_t t) { put_be32(struct_, t); }

void FdtWriter::pad() {
  while (struct_.size() % 4) struct_.push_back(0);
}

uint32_t FdtWriter::string_offset(std::string_view name) {
  if (auto it = string_offsets_.find(name); it != string_offsets_.end()) return it->second;
  const auto offset = static_cast<uint32_t>(strings_.size());
  strings_.append(name);
  strings_.push_back('\0');
  string_offsets_.emplace(std::string(name), offset);
  return offset;
}

void FdtWriter::begin_node(std::string_view name) {
  token(kBeginNode);
  struct_.insert(struct_.end(), name.begin(), name.end());
  struct_.push_back(0);
  pad();
  ++depth_;
}

void FdtWriter::end_node() {
  token(kEndNode);
  --depth_;
}

void FdtWriter::prop(std::string_view name, std::span<const uint8_t> data) {
  token(kProp);
  put_be32(struct_, static_cast<uint32_t>(data.size()));
  put_be32(struct_, string_offset(name));
  struct_.insert(struct_.end(), data.begin(), data.end());
  pad();
}

void FdtWriter::prop_empty(std::string_view name) { prop(name, {}); }

void FdtWriter::prop_u32(std::string_view name, uint32_t value) { prop_cells(name, {value}); }

void FdtWriter::prop_u64(std::string_view name, uint64_t value) {
  prop_cells(name, {static_cast<uint32_t>(value >> 32), static_cast<uint32_t>(value)});
}

void FdtWriter::prop_cells(std::string_view name, std::initializer_list<uint32_t> cells) {
  prop_cells(name, std::span<const uint32_t>(cells.begin(), cells.size()));
}

void FdtWriter::prop_cells(std::string_view name, std::span<const uint32_t> cells) {
  std::vector<uint8_t> data;
  data.reserve(cells.size() * 4);
  for (uint32_t c : cells) put_be32(data, c);
  prop(name, data);
}

void FdtWriter::prop_string(std::string_view name, std::string_view value) {
  std::vector<uint8_t> data(value.begin(), value.end());
  data.push_back(0);
  prop(name, data);
}

void FdtWriter::prop_strings(std::string_view name,
                             std::initializer_list<std::string_view> values) {
  std::vector<uint8_t> data;
  for (auto v : values) {
    data.insert(data.end(), v.begin(), v.end());
    data.push_back(0);
  }
  prop(name, data);
}

Result<std::vector<uint8_t>> FdtWriter::finish() {
  if (depth_ != 0) return make_error(std::format("FDT has {} unclosed node(s)", depth_));
  token(kEnd);

  const uint32_t off_rsvmap = kHeaderSize;
  const uint32_t off_struct = off_rsvmap + kReserveMapSize;
  const auto size_struct = static_cast<uint32_t>(struct_.size());
  const uint32_t off_strings = off_struct + size_struct;
  const auto size_strings = static_cast<uint32_t>(strings_.size());
  const uint32_t total = off_strings + size_strings;

  std::vector<uint8_t> out;
  out.reserve(total);
  for (uint32_t v : {kMagic, total, off_struct, off_strings, off_rsvmap, /*version*/ 17u,
                     /*last_comp_version*/ 16u, /*boot_cpuid_phys*/ 0u, size_strings,
                     size_struct}) {
    put_be32(out, v);
  }
  out.resize(out.size() + kReserveMapSize, 0);
  out.insert(out.end(), struct_.begin(), struct_.end());
  out.insert(out.end(), strings_.begin(), strings_.end());
  return out;
}

}  // namespace kestrel
