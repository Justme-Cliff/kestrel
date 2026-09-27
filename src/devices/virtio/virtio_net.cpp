#include "devices/virtio/virtio_net.h"

#include "core/log.h"

namespace kestrel {
namespace {

constexpr uint64_t kFeatureMac = 1ull << 5;
constexpr uint64_t kFeatureStatus = 1ull << 16;
constexpr uint16_t kLinkUp = 1;
constexpr size_t kMaxPendingRx = 4096;
constexpr size_t kMaxFrame = 65535;

}  // namespace

VirtioNet::VirtioNet(net::Mac mac, TxFn on_tx) : mac_(mac), on_tx_(std::move(on_tx)) {}

uint64_t VirtioNet::features() const { return kFeatureMac | kFeatureStatus; }

void VirtioNet::read_config(uint64_t offset, std::span<uint8_t> out) {
  // Config (§5.1.4): mac[6], status le16.
  uint8_t config[8];
  std::copy(mac_.begin(), mac_.end(), config);
  config[6] = kLinkUp & 0xff;
  config[7] = kLinkUp >> 8;
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = offset + i < sizeof(config) ? config[offset + i] : 0;
  }
}

void VirtioNet::reset() {
  std::lock_guard lock(rx_mutex_);
  rx_pending_.clear();
}

void VirtioNet::queue_notify(unsigned queue) {
  if (queue == 0) flush_rx();  // new receive buffers
  if (queue == 1) process_tx();
}

void VirtioNet::deliver(std::vector<uint8_t> frame) {
  {
    std::lock_guard lock(rx_mutex_);
    if (rx_pending_.size() >= kMaxPendingRx) return;  // guest not reading: drop
    rx_pending_.push_back(std::move(frame));
  }
  flush_rx();
}

void VirtioNet::flush_rx() {
  std::lock_guard lock(rx_mutex_);
  bool used = false, broken = false;
  access_->with_queue(0, [&](Virtqueue& vq) {
    while (!rx_pending_.empty()) {
      auto chain = vq.pop();
      if (!chain) {
        log(LogLevel::kWarn, "virtio-net rx: {}", chain.error().message);
        broken = true;
        return;
      }
      if (!*chain) return;  // no buffers: keep frames pending
      ChainWriter out(**chain);
      const auto& frame = rx_pending_.front();
      uint8_t hdr[kHeaderSize] = {};
      hdr[10] = 1;  // num_buffers = 1
      if (out.remaining() >= kHeaderSize + frame.size()) {
        out.write(hdr);
        out.write(frame);
      }
      vq.push_used((*chain)->head, out.written());
      rx_pending_.pop_front();
      used = true;
    }
  });
  if (broken) access_->set_needs_reset();
  if (used) access_->signal_used();
}

void VirtioNet::process_tx() {
  bool used = false, broken = false;
  std::vector<uint8_t> frame;
  access_->with_queue(1, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain) {
        log(LogLevel::kWarn, "virtio-net tx: {}", chain.error().message);
        broken = true;
        return;
      }
      if (!*chain) return;
      ChainReader in(**chain);
      const size_t total = in.remaining();
      if (total > kHeaderSize && total - kHeaderSize <= kMaxFrame) {
        uint8_t hdr[kHeaderSize];
        in.read(hdr);
        frame.resize(total - kHeaderSize);
        in.read(frame);
        on_tx_(frame);
      }
      vq.push_used((*chain)->head, 0);
      used = true;
    }
  });
  if (broken) access_->set_needs_reset();
  if (used) access_->signal_used();
}

}  // namespace kestrel
