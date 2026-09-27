#include "devices/virtio/virtio_vsock.h"

#include <cstring>

#include "core/log.h"

namespace kestrel {
namespace {

void encode(const VsockPacket& p, uint32_t len, uint8_t* h) {
  std::memcpy(h + 0, &p.src_cid, 8);
  std::memcpy(h + 8, &p.dst_cid, 8);
  std::memcpy(h + 16, &p.src_port, 4);
  std::memcpy(h + 20, &p.dst_port, 4);
  std::memcpy(h + 24, &len, 4);
  std::memcpy(h + 28, &p.type, 2);
  std::memcpy(h + 30, &p.op, 2);
  std::memcpy(h + 32, &p.flags, 4);
  std::memcpy(h + 36, &p.buf_alloc, 4);
  std::memcpy(h + 40, &p.fwd_cnt, 4);
}

}  // namespace

void VirtioVsock::read_config(uint64_t offset, std::span<uint8_t> out) {
  // struct virtio_vsock_config { le64 guest_cid; }
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = offset + i < 8 ? reinterpret_cast<const uint8_t*>(&guest_cid_)[offset + i] : 0;
  }
}

void VirtioVsock::reset() {
  std::lock_guard lock(rx_mutex_);
  rx_pending_.clear();
}

void VirtioVsock::deliver(VsockPacket packet) {
  {
    std::lock_guard lock(rx_mutex_);
    if (rx_pending_.size() >= kMaxPending) return;  // guest stalled; credit keeps this rare
    rx_pending_.push_back(std::move(packet));
  }
  flush_rx();
}

void VirtioVsock::flush_rx() {
  std::lock_guard lock(rx_mutex_);
  bool used = false;
  access_->with_queue(0, [&](Virtqueue& vq) {
    while (!rx_pending_.empty()) {
      auto chain = vq.pop();
      if (!chain || !*chain) return;
      ChainWriter out(**chain);
      VsockPacket& p = rx_pending_.front();
      if (out.remaining() < VsockPacket::kHeaderSize) {  // unusable buffer
        vq.push_used((*chain)->head, 0);
        used = true;
        continue;
      }
      const size_t room = out.remaining() - VsockPacket::kHeaderSize;
      const size_t n = std::min(room, p.payload.size());
      uint8_t hdr[VsockPacket::kHeaderSize];
      encode(p, static_cast<uint32_t>(n), hdr);
      out.write(hdr);
      out.write(std::span(p.payload).first(n));
      vq.push_used((*chain)->head, out.written());
      used = true;
      if (n < p.payload.size()) {
        p.payload.erase(p.payload.begin(), p.payload.begin() + static_cast<long>(n));  // rest next
      } else {
        rx_pending_.pop_front();
      }
    }
  });
  if (used) access_->signal_used();
}

void VirtioVsock::queue_notify(unsigned queue) {
  if (queue == 0) {
    flush_rx();
    return;
  }
  if (queue != 1) return;  // event queue: we never send events; buffers stay posted
  bool used = false;
  std::vector<VsockPacket> packets;
  access_->with_queue(1, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain || !*chain) return;
      ChainReader in(**chain);
      uint8_t h[VsockPacket::kHeaderSize];
      if (in.read(h)) {
        VsockPacket p{};
        uint32_t len;
        std::memcpy(&p.src_cid, h + 0, 8);
        std::memcpy(&p.dst_cid, h + 8, 8);
        std::memcpy(&p.src_port, h + 16, 4);
        std::memcpy(&p.dst_port, h + 20, 4);
        std::memcpy(&len, h + 24, 4);
        std::memcpy(&p.type, h + 28, 2);
        std::memcpy(&p.op, h + 30, 2);
        std::memcpy(&p.flags, h + 32, 4);
        std::memcpy(&p.buf_alloc, h + 36, 4);
        std::memcpy(&p.fwd_cnt, h + 40, 4);
        // The guest can't impersonate another CID (§5.10.6.1).
        if (len <= in.remaining() && len <= 64 * 1024 && p.src_cid == guest_cid_) {
          p.payload.resize(len);
          in.read(p.payload);
          packets.push_back(std::move(p));
        }
      }
      vq.push_used((*chain)->head, 0);
      used = true;
    }
  });
  if (used) access_->signal_used();
  // Outside the transport lock: the backend may reply through deliver().
  for (auto& p : packets) on_packet_(std::move(p));
}

}  // namespace kestrel
