#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "devices/virtio/virtio_device.h"

namespace kestrel {

// virtio-balloon (virtio 1.2 §5.5) with free page reporting only (§5.5.6.7): the
// guest hands us ranges of RAM it has freed, and we tell the host OS it can take the
// pages back. Guest RAM then shrinks on the host when Android frees memory.
class VirtioBalloon final : public VirtioDevice {
 public:
  // Called with page-aligned host ranges the guest no longer uses.
  using DiscardFn = std::function<void(std::span<uint8_t>)>;
  explicit VirtioBalloon(DiscardFn discard);

  uint32_t device_id() const override { return virtio_id::kBalloon; }
  std::string_view name() const override { return "virtio-balloon"; }
  uint64_t features() const override;
  // inflate, deflate, then the reporting queue. Linux numbers queues without gaps for
  // features that weren't negotiated, so reporting lands on 2; spare slots cover
  // drivers that leave gaps.
  unsigned num_queues() const override { return 5; }
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void write_config(uint64_t offset, std::span<const uint8_t> in) override;
  void queue_notify(unsigned queue) override;

  uint64_t discarded_bytes() const { return discarded_; }

 private:
  DiscardFn discard_;
  uint32_t actual_ = 0;
  uint64_t discarded_ = 0;
  uint64_t logged_ = 0;  // discarded_ at the last log line
};

// Releases whole host pages inside `range` (partial pages at the edges are kept).
void discard_host_pages(std::span<uint8_t> range);

}  // namespace kestrel
