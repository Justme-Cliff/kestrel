#include "devices/virtio/virtio_rng.h"

#include "core/log.h"

namespace kestrel {

VirtioRng::VirtioRng(RandomFn random) : random_(std::move(random)) {}

void VirtioRng::queue_notify(unsigned queue) {
  bool used = false, broken = false;
  access_->with_queue(queue, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain) {
        log(LogLevel::kWarn, "virtio-rng: {}", chain.error().message);
        broken = true;
        return;
      }
      if (!*chain) return;
      ChainWriter out(**chain);
      // Small requests are the norm; cap each at 64 KB to bound time on the vCPU thread.
      for (size_t budget = 64 * 1024; budget > 0;) {
        auto seg = out.next_segment(budget);
        if (seg.empty()) break;
        random_(seg);
        budget -= seg.size();
      }
      vq.push_used((*chain)->head, out.written());
      used = true;
    }
  });
  if (broken) access_->set_needs_reset();
  if (used) access_->signal_used();
}

}  // namespace kestrel
