#pragma once

#include <algorithm>
#include <functional>

#include "devices/virtio/virtio_device.h"

namespace kestrel {

// virtio-rng (virtio 1.2 §5.4): fills guest buffers with host randomness.
class VirtioRng final : public VirtioDevice {
 public:
  using RandomFn = std::function<void(std::span<uint8_t>)>;
  explicit VirtioRng(RandomFn random);

  uint32_t device_id() const override { return virtio_id::kRng; }
  std::string_view name() const override { return "virtio-rng"; }
  uint64_t features() const override { return 0; }
  unsigned num_queues() const override { return 1; }
  void read_config(uint64_t, std::span<uint8_t> out) override { std::ranges::fill(out, 0); }
  void queue_notify(unsigned queue) override;

 private:
  RandomFn random_;
};

}  // namespace kestrel
