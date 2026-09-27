#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "devices/virtio/virtio_device.h"

namespace kestrel {

// One vsock packet (virtio 1.2 §5.10.6): header fields + payload.
struct VsockPacket {
  uint64_t src_cid, dst_cid;
  uint32_t src_port, dst_port;
  uint16_t type, op;
  uint32_t flags, buf_alloc, fwd_cnt;
  std::vector<uint8_t> payload;

  static constexpr size_t kHeaderSize = 44;
  static constexpr uint16_t kTypeStream = 1;
  enum Op : uint16_t {
    kRequest = 1, kResponse = 2, kRst = 3, kShutdown = 4, kRw = 5, kCreditUpdate = 6,
    kCreditRequest = 7,
  };
  static constexpr uint32_t kShutdownRcv = 1, kShutdownSend = 2;
};

// virtio-vsock (virtio 1.2 §5.10): socket-style connections between host (CID 2)
// and guest. The backend (net::VsockHost) gets guest packets through `on_packet`
// and sends with deliver().
class VirtioVsock final : public VirtioDevice {
 public:
  using PacketFn = std::function<void(VsockPacket)>;
  static constexpr uint64_t kHostCid = 2;

  VirtioVsock(uint64_t guest_cid, PacketFn on_packet)
      : guest_cid_(guest_cid), on_packet_(std::move(on_packet)) {}

  uint32_t device_id() const override { return virtio_id::kVsock; }
  std::string_view name() const override { return "virtio-vsock"; }
  uint64_t features() const override { return 0; }
  unsigned num_queues() const override { return 3; }  // rx, tx, event
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void queue_notify(unsigned queue) override;
  void reset() override;

  // Thread-safe. Large payloads are split to fit the guest's receive buffers.
  void deliver(VsockPacket packet);

  static constexpr size_t kMaxPending = 4096;

 private:
  void flush_rx();

  uint64_t guest_cid_;
  PacketFn on_packet_;
  std::mutex rx_mutex_;
  std::deque<VsockPacket> rx_pending_;
};

}  // namespace kestrel
