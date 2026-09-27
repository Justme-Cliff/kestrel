#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/error.h"
#include "core/guest_memory.h"

namespace kestrel {

// A split virtqueue (virtio 1.2 §2.7). All ring memory belongs to the guest and is
// untrusted: every index, address and length is validated before use (docs/DESIGN.md §7.3).
class Virtqueue {
 public:
  struct Buffer {
    std::span<uint8_t> data;  // host view of guest memory
    bool device_writable;
  };
  struct Chain {
    uint16_t head;
    std::vector<Buffer> buffers;
  };

  static constexpr uint16_t kMaxSize = 1024;

  // Validates and caches the ring locations. `size` must be a power of two <= kMaxSize.
  Result<void> configure(const GuestMemory& memory, uint16_t size, Gpa desc, Gpa avail, Gpa used);
  void reset();
  bool ready() const { return size_ != 0; }
  uint16_t size() const { return size_; }

  // Next available descriptor chain, nullopt if none, or an error if the guest
  // wrote a malformed ring (the device should then stop using the queue).
  Result<std::optional<Chain>> pop();

  // Returns a chain to the driver; `written` = bytes the device wrote into it.
  void push_used(uint16_t head, uint32_t written);

 private:
  struct Desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
  };
  struct UsedElem {
    uint32_t id;
    uint32_t len;
  };
  static_assert(sizeof(Desc) == 16 && sizeof(UsedElem) == 8);

  const GuestMemory* memory_ = nullptr;
  uint16_t size_ = 0;
  Desc* desc_ = nullptr;
  uint16_t* avail_ = nullptr;  // flags, idx, ring[size], used_event
  uint16_t* used_hdr_ = nullptr;  // flags, idx
  UsedElem* used_ring_ = nullptr;
  uint16_t last_avail_ = 0;
  uint16_t used_idx_ = 0;
};

// Copies sequentially out of the driver-readable buffers / into the device-writable ones.
class ChainReader {
 public:
  explicit ChainReader(const Virtqueue::Chain& chain);
  bool read(std::span<uint8_t> out);  // false if the chain has too few readable bytes
  size_t remaining() const;
  // Next readable segment of at most `max` bytes (zero-copy); empty when exhausted.
  std::span<uint8_t> next_segment(size_t max);

 private:
  std::vector<std::span<uint8_t>> parts_;
  size_t part_ = 0, offset_ = 0;
};

class ChainWriter {
 public:
  explicit ChainWriter(const Virtqueue::Chain& chain);
  bool write(std::span<const uint8_t> in);
  size_t remaining() const;
  std::span<uint8_t> next_segment(size_t max);
  uint32_t written() const { return written_; }

 private:
  std::vector<std::span<uint8_t>> parts_;
  size_t part_ = 0, offset_ = 0;
  uint32_t written_ = 0;
};

}  // namespace kestrel
