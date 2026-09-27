#include "devices/virtio/virtio_console.h"

#include <vector>

namespace kestrel {

void VirtioConsole::queue_notify(unsigned queue) {
  if (queue == 0) {
    flush_rx();  // new receive buffers
    return;
  }
  // Copy guest output under the transport lock, hand it to output_ after releasing
  // it: the consumer may reply through send(), which takes that lock again.
  bool used = false;
  std::vector<uint8_t> output;
  access_->with_queue(1, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain || !*chain) return;
      ChainReader in(**chain);
      for (auto seg = in.next_segment(4096); !seg.empty(); seg = in.next_segment(4096)) {
        output.insert(output.end(), seg.begin(), seg.end());
      }
      vq.push_used((*chain)->head, 0);
      used = true;
    }
  });
  if (used) access_->signal_used();
  if (output_ && !output.empty()) output_(output);
}

void VirtioConsole::reset() {
  std::lock_guard lock(rx_mutex_);
  rx_pending_.clear();
}

void VirtioConsole::send(std::span<const uint8_t> data) {
  {
    std::lock_guard lock(rx_mutex_);
    if (rx_pending_.size() + data.size() > kMaxPending) return;  // guest not reading
    rx_pending_.insert(rx_pending_.end(), data.begin(), data.end());
  }
  flush_rx();
}

void VirtioConsole::flush_rx() {
  std::lock_guard lock(rx_mutex_);
  bool used = false;
  access_->with_queue(0, [&](Virtqueue& vq) {
    while (!rx_pending_.empty()) {
      auto chain = vq.pop();
      if (!chain || !*chain) return;
      ChainWriter out(**chain);
      while (!rx_pending_.empty()) {
        auto seg = out.next_segment(rx_pending_.size());
        if (seg.empty()) break;
        std::copy_n(rx_pending_.begin(), seg.size(), seg.begin());
        rx_pending_.erase(rx_pending_.begin(), rx_pending_.begin() + static_cast<long>(seg.size()));
      }
      vq.push_used((*chain)->head, out.written());
      used = true;
    }
  });
  if (used) access_->signal_used();
}

}  // namespace kestrel
