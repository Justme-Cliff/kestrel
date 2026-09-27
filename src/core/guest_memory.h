#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "core/error.h"
#include "core/gpa.h"

namespace kestrel {

// Guest RAM: one anonymous mapping. Untouched pages cost no real memory.
// All accessors bounds-check, since guest-provided addresses are untrusted.
class GuestMemory {
 public:
  static constexpr uint64_t kHostPageSize = 16 * 1024;

  static Result<GuestMemory> create(Gpa base, uint64_t size);

  GuestMemory(GuestMemory&& other) noexcept;
  GuestMemory& operator=(GuestMemory&& other) noexcept;
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;
  ~GuestMemory();

  Gpa base() const { return base_; }
  uint64_t size() const { return size_; }
  uint8_t* host_base() const { return host_; }

  // Returns the host view of [gpa, gpa+len) or nullopt if any byte is outside guest RAM.
  std::optional<std::span<uint8_t>> translate(Gpa gpa, uint64_t len) const;

  bool write(Gpa gpa, std::span<const uint8_t> data) const;
  bool read(Gpa gpa, std::span<uint8_t> out) const;

 private:
  GuestMemory(Gpa base, uint64_t size, uint8_t* host) : base_(base), size_(size), host_(host) {}

  Gpa base_;
  uint64_t size_ = 0;
  uint8_t* host_ = nullptr;
};

}  // namespace kestrel
