#pragma once

#include <deque>
#include <mutex>
#include <vector>

#include "devices/virtio/virtio_device.h"
#include "net/packet.h"

namespace kestrel {

// virtio-net (virtio 1.2 §5.1) without offloads: queue 0 = receive, 1 = transmit.
// The backend (e.g. net::UserNet) receives transmitted frames through `on_tx`
// and delivers frames with deliver().
class VirtioNet final : public VirtioDevice {
 public:
  using TxFn = std::function<void(std::span<const uint8_t> frame)>;

  VirtioNet(net::Mac mac, TxFn on_tx);

  uint32_t device_id() const override { return virtio_id::kNet; }
  std::string_view name() const override { return "virtio-net"; }
  uint64_t features() const override;
  unsigned num_queues() const override { return 2; }
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void queue_notify(unsigned queue) override;
  void reset() override;

  // Thread-safe. Frames wait here while the guest has no receive buffers posted.
  void deliver(std::vector<uint8_t> frame);

  static constexpr size_t kHeaderSize = 12;  // struct virtio_net_hdr_v1

 private:
  void flush_rx();
  void process_tx();

  net::Mac mac_;
  TxFn on_tx_;
  std::mutex rx_mutex_;
  std::deque<std::vector<uint8_t>> rx_pending_;
};

}  // namespace kestrel
