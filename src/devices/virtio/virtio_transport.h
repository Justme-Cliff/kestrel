#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "core/guest_memory.h"
#include "devices/virtio/virtio_device.h"

namespace kestrel {

// Transport-independent virtio state (virtio 1.2 §2-3): feature negotiation,
// device status, queue configuration and the interrupt status. VirtioMmio and
// VirtioPci only translate their register layouts onto this.
class VirtioTransport : public VirtioQueueAccess {
 public:
  using IrqFn = std::function<void(bool level)>;

  static constexpr uint32_t kStatusDriverOk = 4;
  static constexpr uint32_t kStatusFeaturesOk = 8;
  static constexpr uint32_t kStatusNeedsReset = 64;
  static constexpr uint32_t kIntUsedBuffer = 1;
  static constexpr uint32_t kIntConfigChange = 2;

  VirtioTransport(const GuestMemory& memory, std::unique_ptr<VirtioDevice> device, IrqFn irq);
  // Destroys the device first: its I/O thread may still call back into us.
  ~VirtioTransport() override;

  VirtioDevice& device() { return *device_; }
  unsigned num_queues() const { return static_cast<unsigned>(queues_.size()); }

  // ---- register-level operations, all thread-safe ----
  uint64_t offered_features() const { return device_->features() | kVirtioFVersion1; }
  uint32_t device_features_word(uint32_t select) const;
  void set_driver_features_word(uint32_t select, uint32_t value);
  uint32_t status();
  void set_status(uint32_t value);  // 0 resets the device

  void select_queue(uint32_t index);
  uint32_t selected_queue();
  bool queue_exists();  // is the selected queue valid?
  uint16_t queue_max_size() const { return device_->queue_max_size(); }
  uint16_t queue_size();
  void set_queue_size(uint16_t size);
  bool queue_ready();
  void set_queue_ready(bool ready);
  enum class Ring { kDesc, kDriver, kDevice };
  uint64_t queue_addr(Ring ring);
  void set_queue_addr_low(Ring ring, uint32_t v);
  void set_queue_addr_high(Ring ring, uint32_t v);

  void notify(uint32_t queue);  // driver kicked a queue

  uint32_t interrupt_status();
  void ack_interrupt(uint32_t bits);
  uint32_t read_and_clear_interrupt();  // PCI ISR semantics

  // VirtioQueueAccess
  bool with_queue(unsigned index, const std::function<void(Virtqueue&)>& fn) override;
  void signal_used() override;
  void set_needs_reset() override;

 private:
  struct Queue {
    uint16_t num = 0;
    uint64_t addr[3] = {};
    bool ready = false;
    Virtqueue vq;
  };

  void reset_locked();
  void update_irq_locked();
  Queue* selected_locked();

  const GuestMemory& memory_;
  std::unique_ptr<VirtioDevice> device_;
  IrqFn irq_;

  std::mutex mutex_;
  std::vector<Queue> queues_;
  uint32_t status_ = 0;
  uint64_t driver_features_ = 0;
  uint32_t queue_sel_ = 0;
  uint32_t interrupt_status_ = 0;
  bool irq_line_ = false;
};

}  // namespace kestrel
