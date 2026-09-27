#include "devices/virtio/virtio_balloon.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

#include "core/log.h"

namespace kestrel {

namespace {
constexpr uint64_t kFeatureReporting = 1ull << 5;  // VIRTIO_BALLOON_F_REPORTING
constexpr unsigned kInflateQueue = 0, kDeflateQueue = 1;
}  // namespace

void discard_host_pages(std::span<uint8_t> range) {
  static const uintptr_t page = static_cast<uintptr_t>(getpagesize());  // 16 KB on Apple Silicon
  const uintptr_t begin = (reinterpret_cast<uintptr_t>(range.data()) + page - 1) & ~(page - 1);
  const uintptr_t end = (reinterpret_cast<uintptr_t>(range.data()) + range.size()) & ~(page - 1);
  if (end <= begin) return;
#ifdef MADV_FREE_REUSABLE
  // macOS: drops the pages from our memory footprint right away.
  madvise(reinterpret_cast<void*>(begin), end - begin, MADV_FREE_REUSABLE);
#else
  madvise(reinterpret_cast<void*>(begin), end - begin, MADV_DONTNEED);
#endif
}

VirtioBalloon::VirtioBalloon(DiscardFn discard) : discard_(std::move(discard)) {}

uint64_t VirtioBalloon::features() const { return kFeatureReporting; }

void VirtioBalloon::read_config(uint64_t offset, std::span<uint8_t> out) {
  // §5.5.4: num_pages (we never ask for memory), actual, free_page_hint_cmd_id, poison_val.
  const uint32_t config[4] = {0, actual_, 0, 0};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = offset + i < sizeof(config) ? reinterpret_cast<const uint8_t*>(config)[offset + i] : 0;
  }
}

void VirtioBalloon::write_config(uint64_t offset, std::span<const uint8_t> in) {
  if (offset == 4 && in.size() == 4) actual_ = static_cast<uint32_t>(in[0] | in[1] << 8 | in[2] << 16 | in[3] << 24);
}

void VirtioBalloon::queue_notify(unsigned queue) {
  bool used = false, broken = false;
  access_->with_queue(queue, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain) {
        log(LogLevel::kWarn, "virtio-balloon: {}", chain.error().message);
        broken = true;
        return;
      }
      if (!*chain) return;
      // Inflate/deflate aren't used (num_pages stays 0): just hand buffers back.
      // Reporting buffers *are* the free guest pages, already validated as guest RAM.
      if (queue != kInflateQueue && queue != kDeflateQueue) {
        for (const auto& b : (*chain)->buffers) {
          if (b.device_writable) continue;
          discard_(b.data);
          discarded_ += b.data.size();
          if (discarded_ - logged_ >= (512ull << 20)) {
            logged_ = discarded_;
            log(LogLevel::kInfo, "virtio-balloon: {} MB of freed Android memory returned to macOS so far",
                discarded_ >> 20);
          }
        }
      }
      vq.push_used((*chain)->head, 0);
      used = true;
    }
  });
  if (broken) access_->set_needs_reset();
  if (used) access_->signal_used();
}

}  // namespace kestrel
