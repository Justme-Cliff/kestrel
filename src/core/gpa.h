#pragma once

#include <compare>
#include <cstdint>

namespace kestrel {

// Guest physical address. A distinct type so it can never be mixed up with a host pointer.
struct Gpa {
  uint64_t value = 0;

  constexpr Gpa() = default;
  constexpr explicit Gpa(uint64_t v) : value(v) {}
  constexpr auto operator<=>(const Gpa&) const = default;
  constexpr Gpa operator+(uint64_t offset) const { return Gpa(value + offset); }
};

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace kestrel
