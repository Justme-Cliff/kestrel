#include "devices/virtio/virtio_blk.h"
#include "core/thread_priority.h"

#include <cstring>
#include <vector>

#include "core/log.h"

namespace kestrel {
namespace {

constexpr uint64_t kFeatureRo = 1ull << 5;
constexpr uint64_t kFeatureFlush = 1ull << 9;

constexpr uint32_t kTypeIn = 0;
constexpr uint32_t kTypeOut = 1;
constexpr uint32_t kTypeFlush = 4;
constexpr uint32_t kTypeGetId = 8;

constexpr uint8_t kStatusOk = 0;
constexpr uint8_t kStatusIoErr = 1;
constexpr uint8_t kStatusUnsupported = 2;

struct RequestHeader {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};
static_assert(sizeof(RequestHeader) == 16);

}  // namespace

VirtioBlk::VirtioBlk(std::unique_ptr<BlockFile> file) : file_(std::move(file)) {
  io_thread_ = std::thread([this] { io_loop(); });
}

VirtioBlk::~VirtioBlk() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  io_thread_.join();
}

uint64_t VirtioBlk::features() const {
  return kFeatureFlush | (file_->read_only() ? kFeatureRo : 0);
}

void VirtioBlk::read_config(uint64_t offset, std::span<uint8_t> out) {
  // Config layout (§5.2.4): only `capacity` (le64 sectors at offset 0) is offered.
  uint8_t config[8];
  const uint64_t capacity = file_->size() / kSectorSize;
  std::memcpy(config, &capacity, 8);
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = offset + i < sizeof(config) ? config[offset + i] : 0;
  }
}

void VirtioBlk::queue_notify(unsigned /*queue*/) {
  {
    std::lock_guard lock(mutex_);
    pending_ = true;
  }
  cv_.notify_one();
}

void VirtioBlk::io_loop() {
  set_thread_role(ThreadRole::kIo);
  while (true) {
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return pending_ || quit_; });
      if (quit_) return;
      pending_ = false;
    }
    // Pop under the transport lock, do I/O without it, then complete.
    while (true) {
      std::vector<Virtqueue::Chain> batch;
      bool broken = false;
      access_->with_queue(0, [&](Virtqueue& vq) {
        while (batch.size() < 64) {
          auto chain = vq.pop();
          if (!chain) {
            log(LogLevel::kWarn, "virtio-blk: {}", chain.error().message);
            broken = true;
            return;
          }
          if (!*chain) return;
          batch.push_back(std::move(**chain));
        }
      });
      if (broken) access_->set_needs_reset();
      if (batch.empty()) break;
      std::vector<std::pair<uint16_t, uint32_t>> done;
      done.reserve(batch.size());
      for (const auto& chain : batch) done.emplace_back(chain.head, handle_request(chain));
      access_->with_queue(0, [&](Virtqueue& vq) {
        for (auto [head, written] : done) vq.push_used(head, written);
      });
      access_->signal_used();
    }
  }
}

uint32_t VirtioBlk::handle_request(const Virtqueue::Chain& chain) {
  ChainReader in(chain);
  ChainWriter out(chain);
  RequestHeader hdr;
  // The last writable byte is the status; data comes before it.
  if (!in.read(std::span(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr))) || out.remaining() < 1) {
    return 0;  // malformed: nowhere to put a status
  }
  const size_t data_out = out.remaining() - 1;
  uint8_t status = kStatusOk;
  const uint64_t capacity = file_->size() / kSectorSize;

  auto in_range = [&](uint64_t bytes) {
    return hdr.sector <= capacity && bytes <= (capacity - hdr.sector) * kSectorSize;
  };

  switch (hdr.type) {
    case kTypeIn: {
      if (!in_range(data_out)) {
        status = kStatusIoErr;
        break;
      }
      uint64_t pos = hdr.sector * kSectorSize;
      for (size_t left = data_out; left > 0;) {
        auto seg = out.next_segment(left);
        if (!file_->read_at(pos, seg)) {
          status = kStatusIoErr;
          break;
        }
        pos += seg.size();
        left -= seg.size();
      }
      break;
    }
    case kTypeOut: {
      const size_t len = in.remaining();
      if (file_->read_only()) {
        status = kStatusIoErr;
        break;
      }
      if (!in_range(len)) {
        status = kStatusIoErr;
        break;
      }
      uint64_t pos = hdr.sector * kSectorSize;
      for (auto seg = in.next_segment(len); !seg.empty(); seg = in.next_segment(len)) {
        if (!file_->write_at(pos, seg)) {
          status = kStatusIoErr;
          break;
        }
        pos += seg.size();
      }
      break;
    }
    case kTypeFlush:
      if (!file_->flush()) status = kStatusIoErr;
      break;
    case kTypeGetId: {
      uint8_t id[20] = {};
      std::memcpy(id, "kestrel-disk", 12);
      auto seg = out.next_segment(std::min<size_t>(data_out, sizeof(id)));
      std::memcpy(seg.data(), id, seg.size());
      break;
    }
    default:
      status = kStatusUnsupported;
      break;
  }
  // Skip unused data space so the status lands in the last writable byte.
  for (size_t left = out.remaining() - 1; left > 0;) left -= out.next_segment(left).size();
  out.write(std::span(&status, 1));
  return out.written();
}

}  // namespace kestrel
