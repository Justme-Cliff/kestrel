#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "devices/virtio/virtio_device.h"
#include "host/block_file.h"

namespace kestrel {

// virtio-blk (virtio 1.2 §5.2). Requests run on a dedicated I/O thread so a slow
// disk never blocks a vCPU (docs/DESIGN.md §7.4).
class VirtioBlk final : public VirtioDevice {
 public:
  explicit VirtioBlk(std::unique_ptr<BlockFile> file);
  ~VirtioBlk() override;

  uint32_t device_id() const override { return virtio_id::kBlock; }
  std::string_view name() const override { return "virtio-blk"; }
  uint64_t features() const override;
  unsigned num_queues() const override { return 1; }
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void queue_notify(unsigned queue) override;

  static constexpr uint32_t kSectorSize = 512;

 private:
  void io_loop();
  // Handles one request; returns bytes written into the chain.
  uint32_t handle_request(const Virtqueue::Chain& chain);

  std::unique_ptr<BlockFile> file_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool pending_ = false;
  bool quit_ = false;
  std::thread io_thread_;
};

}  // namespace kestrel
