#pragma once

#include <map>
#include <mutex>
#include <vector>

#include "core/guest_memory.h"
#include "devices/virtio/virtio_device.h"
#include "host/display.h"

namespace kestrel {

// virtio-gpu in 2D mode (virtio 1.2 §5.7): the guest renders in software and
// hands us finished frames. 3D (gfxstream) comes in Phase 5.
class VirtioGpu final : public VirtioDevice {
 public:
  VirtioGpu(const GuestMemory& memory, uint32_t width, uint32_t height, DisplaySink* sink);

  uint32_t device_id() const override { return virtio_id::kGpu; }
  std::string_view name() const override { return "virtio-gpu"; }
  uint64_t features() const override { return 0; }
  unsigned num_queues() const override { return 2; }  // control, cursor
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void write_config(uint64_t offset, std::span<const uint8_t> in) override;
  void queue_notify(unsigned queue) override;
  void reset() override;

  static constexpr uint64_t kMaxResourceBytes = 64ull << 20;
  static constexpr uint64_t kMaxTotalBytes = 512ull << 20;

 private:
  struct Resource {
    uint32_t format, width, height;
    std::vector<uint8_t> pixels;           // host copy, width * 4 bytes per row
    std::vector<std::span<uint8_t>> backing;  // guest pages, in order
    uint64_t backing_size = 0;
  };

  // Handles one control command; returns the response bytes.
  std::vector<uint8_t> handle(std::span<const uint8_t> request, ChainReader& extra);
  uint32_t transfer_to_host(std::span<const uint8_t> req);
  uint32_t attach_backing(std::span<const uint8_t> req, ChainReader& extra);
  uint32_t flush(std::span<const uint8_t> req);

  const GuestMemory& memory_;
  uint32_t width_, height_;
  DisplaySink* sink_;

  std::mutex mutex_;
  std::map<uint32_t, Resource> resources_;
  uint64_t total_bytes_ = 0;
  uint32_t scanout_resource_ = 0;
  uint32_t scanout_x_ = 0, scanout_y_ = 0, scanout_w_ = 0, scanout_h_ = 0;  // visible rect
  uint32_t events_read_ = 0;
};

}  // namespace kestrel
