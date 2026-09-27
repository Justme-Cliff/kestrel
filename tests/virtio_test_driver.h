#pragma once

// A minimal virtio "driver" for tests: lays out one split queue in guest memory
// and submits descriptor chains, like a guest kernel would.

#include <cstring>
#include <vector>

#include "core/guest_memory.h"

namespace kestrel::test {

class QueueBuilder {
 public:
  static constexpr uint64_t kBase = 0x40000000;
  static constexpr uint16_t kSize = 16;
  static constexpr uint64_t kDesc = kBase + 0x0000;
  static constexpr uint64_t kAvail = kBase + 0x1000;
  static constexpr uint64_t kUsed = kBase + 0x2000;
  static constexpr uint64_t kData = kBase + 0x10000;

  explicit QueueBuilder(GuestMemory& mem) : mem_(mem) {}

  struct Seg {
    uint64_t addr;
    uint32_t len;
    bool writable;
  };

  // Writes descriptors for `segs` and publishes the chain. Returns the head index.
  uint16_t submit(const std::vector<Seg>& segs) {
    const uint16_t head = next_desc_;
    for (size_t i = 0; i < segs.size(); ++i) {
      const uint16_t idx = next_desc_++;
      uint16_t flags = (segs[i].writable ? 2 : 0) | (i + 1 < segs.size() ? 1 : 0);
      write_desc(idx, segs[i].addr, segs[i].len, flags, static_cast<uint16_t>(idx + 1));
    }
    put16(kAvail + 4 + 2 * (avail_idx_ % kSize), head);
    ++avail_idx_;
    put16(kAvail + 2, avail_idx_);
    return head;
  }

  void write_desc(uint16_t idx, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t d[16];
    std::memcpy(d, &addr, 8);
    std::memcpy(d + 8, &len, 4);
    std::memcpy(d + 12, &flags, 2);
    std::memcpy(d + 14, &next, 2);
    mem_.write(Gpa(kDesc + 16ull * idx), d);
  }

  void publish_raw(uint16_t head) {
    put16(kAvail + 4 + 2 * (avail_idx_ % kSize), head);
    ++avail_idx_;
    put16(kAvail + 2, avail_idx_);
  }

  uint16_t used_idx() { return get16(kUsed + 2); }
  uint32_t used_len(uint16_t slot) { return get32(kUsed + 4 + 8ull * slot + 4); }

  void put16(uint64_t a, uint16_t v) { mem_.write(Gpa(a), std::span(reinterpret_cast<uint8_t*>(&v), 2)); }
  uint16_t get16(uint64_t a) {
    uint16_t v;
    mem_.read(Gpa(a), std::span(reinterpret_cast<uint8_t*>(&v), 2));
    return v;
  }
  uint32_t get32(uint64_t a) {
    uint32_t v;
    mem_.read(Gpa(a), std::span(reinterpret_cast<uint8_t*>(&v), 4));
    return v;
  }

 private:
  GuestMemory& mem_;
  uint16_t next_desc_ = 0;
  uint16_t avail_idx_ = 0;
};

}  // namespace kestrel::test
