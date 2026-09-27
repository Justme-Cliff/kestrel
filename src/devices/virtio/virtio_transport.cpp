#include "devices/virtio/virtio_transport.h"

#include "core/log.h"

namespace kestrel {

VirtioTransport::VirtioTransport(const GuestMemory& memory, std::unique_ptr<VirtioDevice> device,
                                 IrqFn irq)
    : memory_(memory), device_(std::move(device)), irq_(std::move(irq)) {
  queues_.resize(device_->num_queues());
  device_->bind(this);
}

VirtioTransport::~VirtioTransport() { device_.reset(); }

VirtioTransport::Queue* VirtioTransport::selected_locked() {
  return queue_sel_ < queues_.size() ? &queues_[queue_sel_] : nullptr;
}

void VirtioTransport::update_irq_locked() {
  const bool level = interrupt_status_ != 0;
  if (level != irq_line_) {
    irq_line_ = level;
    irq_(level);
  }
}

void VirtioTransport::reset_locked() {
  for (auto& q : queues_) q = Queue{};
  status_ = 0;
  driver_features_ = 0;
  queue_sel_ = 0;
  interrupt_status_ = 0;
  update_irq_locked();
}

uint32_t VirtioTransport::device_features_word(uint32_t select) const {
  const uint64_t f = offered_features();
  return select == 0 ? static_cast<uint32_t>(f) : select == 1 ? static_cast<uint32_t>(f >> 32) : 0;
}

void VirtioTransport::set_driver_features_word(uint32_t select, uint32_t value) {
  std::lock_guard lock(mutex_);
  if (select == 0) driver_features_ = (driver_features_ & ~0xffffffffull) | value;
  if (select == 1) driver_features_ = (driver_features_ & 0xffffffffull) | (uint64_t(value) << 32);
}

uint32_t VirtioTransport::status() {
  std::lock_guard lock(mutex_);
  return status_;
}

void VirtioTransport::set_status(uint32_t v) {
  std::unique_lock lock(mutex_);
  if (v == 0) {
    reset_locked();
    lock.unlock();
    device_->reset();
    return;
  }
  if ((v & kStatusFeaturesOk) && !(status_ & kStatusFeaturesOk)) {
    // Refuse FEATURES_OK if the driver asked for something we didn't offer (§3.1.1).
    const uint64_t offered = offered_features();
    if ((driver_features_ & ~offered) != 0 || !(driver_features_ & kVirtioFVersion1)) {
      status_ = v & ~kStatusFeaturesOk;
      return;
    }
  }
  status_ = v;
}

void VirtioTransport::select_queue(uint32_t index) {
  std::lock_guard lock(mutex_);
  queue_sel_ = index;
}

uint32_t VirtioTransport::selected_queue() {
  std::lock_guard lock(mutex_);
  return queue_sel_;
}

bool VirtioTransport::queue_exists() {
  std::lock_guard lock(mutex_);
  return selected_locked() != nullptr;
}

uint16_t VirtioTransport::queue_size() {
  std::lock_guard lock(mutex_);
  auto* q = selected_locked();
  return q ? (q->num ? q->num : device_->queue_max_size()) : 0;
}

void VirtioTransport::set_queue_size(uint16_t size) {
  std::lock_guard lock(mutex_);
  if (auto* q = selected_locked(); q && !q->ready) q->num = size;
}

bool VirtioTransport::queue_ready() {
  std::lock_guard lock(mutex_);
  auto* q = selected_locked();
  return q && q->ready;
}

void VirtioTransport::set_queue_ready(bool ready) {
  std::lock_guard lock(mutex_);
  auto* q = selected_locked();
  if (!q) return;
  if (!ready) {
    q->ready = false;
    q->vq.reset();
    return;
  }
  const uint16_t num = q->num ? q->num : device_->queue_max_size();
  if (num > device_->queue_max_size()) {
    log(LogLevel::kWarn, "{}: queue size {} above max", device_->name(), num);
    status_ |= kStatusNeedsReset;
    return;
  }
  auto r = q->vq.configure(memory_, num, Gpa(q->addr[0]), Gpa(q->addr[1]), Gpa(q->addr[2]));
  if (!r) {
    log(LogLevel::kWarn, "{}: {}", device_->name(), r.error().message);
    status_ |= kStatusNeedsReset;
    return;
  }
  q->ready = true;
}

uint64_t VirtioTransport::queue_addr(Ring ring) {
  std::lock_guard lock(mutex_);
  auto* q = selected_locked();
  return q ? q->addr[static_cast<int>(ring)] : 0;
}

void VirtioTransport::set_queue_addr_low(Ring ring, uint32_t v) {
  std::lock_guard lock(mutex_);
  if (auto* q = selected_locked(); q && !q->ready) {
    uint64_t& a = q->addr[static_cast<int>(ring)];
    a = (a & ~0xffffffffull) | v;
  }
}

void VirtioTransport::set_queue_addr_high(Ring ring, uint32_t v) {
  std::lock_guard lock(mutex_);
  if (auto* q = selected_locked(); q && !q->ready) {
    uint64_t& a = q->addr[static_cast<int>(ring)];
    a = (a & 0xffffffffull) | (uint64_t(v) << 32);
  }
}

void VirtioTransport::notify(uint32_t queue) {
  // Outside the lock: the device takes it again through with_queue().
  if (queue < queues_.size()) device_->queue_notify(queue);
}

uint32_t VirtioTransport::interrupt_status() {
  std::lock_guard lock(mutex_);
  return interrupt_status_;
}

void VirtioTransport::ack_interrupt(uint32_t bits) {
  std::lock_guard lock(mutex_);
  interrupt_status_ &= ~bits;
  update_irq_locked();
}

uint32_t VirtioTransport::read_and_clear_interrupt() {
  std::lock_guard lock(mutex_);
  const uint32_t v = interrupt_status_;
  interrupt_status_ = 0;
  update_irq_locked();
  return v;
}

bool VirtioTransport::with_queue(unsigned index, const std::function<void(Virtqueue&)>& fn) {
  std::lock_guard lock(mutex_);
  if (!(status_ & kStatusDriverOk) || index >= queues_.size() || !queues_[index].ready) {
    return false;
  }
  fn(queues_[index].vq);
  return true;
}

void VirtioTransport::signal_used() {
  std::lock_guard lock(mutex_);
  interrupt_status_ |= kIntUsedBuffer;
  update_irq_locked();
}

void VirtioTransport::set_needs_reset() {
  std::lock_guard lock(mutex_);
  status_ |= kStatusNeedsReset;
  interrupt_status_ |= kIntConfigChange;
  update_irq_locked();
}

}  // namespace kestrel
