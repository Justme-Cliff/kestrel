#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

namespace kestrel {

// Builds a flattened device tree blob (Devicetree Specification v0.4, chapter 5).
class FdtWriter {
 public:
  FdtWriter();

  void begin_node(std::string_view name);
  void end_node();

  void prop(std::string_view name, std::span<const uint8_t> data);
  void prop_empty(std::string_view name);
  void prop_u32(std::string_view name, uint32_t value);
  void prop_u64(std::string_view name, uint64_t value);
  void prop_cells(std::string_view name, std::initializer_list<uint32_t> cells);
  void prop_cells(std::string_view name, std::span<const uint32_t> cells);
  void prop_string(std::string_view name, std::string_view value);
  void prop_strings(std::string_view name, std::initializer_list<std::string_view> values);

  // Fails if nodes are left open.
  Result<std::vector<uint8_t>> finish();

 private:
  void token(uint32_t t);
  void pad();
  uint32_t string_offset(std::string_view name);

  std::vector<uint8_t> struct_;
  std::string strings_;
  std::map<std::string, uint32_t, std::less<>> string_offsets_;
  int depth_ = 0;
};

}  // namespace kestrel
