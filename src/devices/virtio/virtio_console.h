#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <mutex>

#include "devices/virtio/virtio_device.h"

namespace kestrel {

// Single-port virtio-console (virtio 1.2 §5.3), one per Android /dev/hvcN.
// Guest output goes to `output`; host data goes to the guest through send().
class VirtioConsole final : public VirtioDevice {
 public:
  using OutputFn = std::function<void(std::span<const uint8_t>)>;
  explicit VirtioConsole(OutputFn output) : output_(std::move(output)) {}

  uint32_t device_id() const override { return virtio_id::kConsole; }
  std::string_view name() const override { return "virtio-console"; }
  uint64_t features() const override { return 0; }
  unsigned num_queues() const override { return 2; }  // receiveq, transmitq
  void read_config(uint64_t, std::span<uint8_t> out) override { std::ranges::fill(out, 0); }
  void queue_notify(unsigned queue) override;
  void reset() override;

  // Thread-safe. Bytes wait until the guest posts receive buffers.
  void send(std::span<const uint8_t> data);
  // Set before the guest starts; called on a vCPU thread for guest output.
  void set_output(OutputFn output) { output_ = std::move(output); }

  static constexpr size_t kMaxPending = 1 << 20;

 private:
  void flush_rx();

  OutputFn output_;
  std::mutex rx_mutex_;
  std::deque<uint8_t> rx_pending_;
};

}  // namespace kestrel
