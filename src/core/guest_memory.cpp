#include "core/guest_memory.h"

#include <sys/mman.h>

#include <cstring>
#include <format>

namespace kestrel {

Result<GuestMemory> GuestMemory::create(Gpa base, uint64_t size) {
  if (size == 0 || base.value % kHostPageSize != 0) {
    return make_error("guest RAM base must be 16 KB aligned and size non-zero");
  }
  size = align_up(size, kHostPageSize);
  void* host = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (host == MAP_FAILED) {
    return make_error(std::format("mmap of {} bytes of guest RAM failed", size));
  }
  return GuestMemory(base, size, static_cast<uint8_t*>(host));
}

GuestMemory::GuestMemory(GuestMemory&& other) noexcept
    : base_(other.base_), size_(other.size_), host_(other.host_) {
  other.host_ = nullptr;
  other.size_ = 0;
}

GuestMemory& GuestMemory::operator=(GuestMemory&& other) noexcept {
  if (this != &other) {
    if (host_) munmap(host_, size_);
    base_ = other.base_;
    size_ = other.size_;
    host_ = other.host_;
    other.host_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

GuestMemory::~GuestMemory() {
  if (host_) munmap(host_, size_);
}

std::optional<std::span<uint8_t>> GuestMemory::translate(Gpa gpa, uint64_t len) const {
  if (gpa.value < base_.value) return std::nullopt;
  const uint64_t offset = gpa.value - base_.value;
  // Written to avoid overflow in offset + len.
  if (offset > size_ || len > size_ - offset) return std::nullopt;
  return std::span<uint8_t>(host_ + offset, len);
}

bool GuestMemory::write(Gpa gpa, std::span<const uint8_t> data) const {
  auto dst = translate(gpa, data.size());
  if (!dst) return false;
  std::memcpy(dst->data(), data.data(), data.size());
  return true;
}

bool GuestMemory::read(Gpa gpa, std::span<uint8_t> out) const {
  auto src = translate(gpa, out.size());
  if (!src) return false;
  std::memcpy(out.data(), src->data(), out.size());
  return true;
}

}  // namespace kestrel
