#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "devices/virtio/virtqueue.h"

namespace kestrel {

// virtio 1.2 device IDs (§5).
namespace virtio_id {
constexpr uint32_t kNet = 1;
constexpr uint32_t kBlock = 2;
constexpr uint32_t kConsole = 3;
constexpr uint32_t kRng = 4;
constexpr uint32_t kBalloon = 5;
constexpr uint32_t kGpu = 16;
constexpr uint32_t kInput = 18;
constexpr uint32_t kVsock = 19;
constexpr uint32_t kSound = 25;
}  // namespace virtio_id

constexpr uint64_t kVirtioFVersion1 = 1ull << 32;

// Given to a device by its transport. Thread-safe.
class VirtioQueueAccess {
 public:
  virtual ~VirtioQueueAccess() = default;
  // Runs `fn` with exclusive access to queue `index`. Returns false if the queue isn't live.
  virtual bool with_queue(unsigned index, const std::function<void(Virtqueue&)>& fn) = 0;
  // Tells the driver that used buffers are ready (raises the interrupt).
  virtual void signal_used() = 0;
  // Marks the device broken (virtio 1.2 §2.1.2) after a malformed request.
  virtual void set_needs_reset() = 0;
};

// A virtio device, independent of transport (MMIO now, PCI later).
class VirtioDevice {
 public:
  virtual ~VirtioDevice() = default;
  virtual uint32_t device_id() const = 0;
  virtual std::string_view name() const = 0;
  // Device-specific feature bits; the transport adds VIRTIO_F_VERSION_1.
  virtual uint64_t features() const = 0;
  virtual unsigned num_queues() const = 0;
  virtual uint16_t queue_max_size() const { return 256; }
  virtual void read_config(uint64_t offset, std::span<uint8_t> out) = 0;
  virtual void write_config(uint64_t /*offset*/, std::span<const uint8_t> /*in*/) {}

  // Called once by the transport before any other call.
  virtual void bind(VirtioQueueAccess* access) { access_ = access; }
  // The driver added buffers to `queue`. Called on a vCPU thread: must not block.
  virtual void queue_notify(unsigned queue) = 0;
  // The driver reset the device (status 0).
  virtual void reset() {}

  // Shared memory region (virtio 1.2 §2.10), e.g. virtio-gpu host-visible memory.
  // The transport exposes it (a PCI BAR) and tells the device where the guest put it.
  virtual uint64_t shared_memory_size() const { return 0; }
  virtual uint8_t shared_memory_id() const { return 0; }
  virtual void bind_shared_memory(std::function<std::optional<uint64_t>()> guest_base) {
    (void)guest_base;
  }

 protected:
  VirtioQueueAccess* access_ = nullptr;
};

}  // namespace kestrel
