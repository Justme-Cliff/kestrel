#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "core/mmio_device.h"

namespace kestrel {

// Arm PrimeCell PL031 real-time clock (DDI 0224). Gives the guest the wall-clock time.
class Pl031 final : public MmioDevice {
 public:
  using ClockFn = std::function<uint32_t()>;  // host time, seconds since the Unix epoch
  using IrqFn = std::function<void(bool level)>;

  Pl031(ClockFn clock, IrqFn irq);

  std::string_view name() const override { return "pl031"; }
  uint64_t mmio_read(uint64_t offset, unsigned size) override;
  void mmio_write(uint64_t offset, unsigned size, uint64_t value) override;

 private:
  uint32_t now_locked() const { return clock_() + offset_; }
  void update_irq_locked();

  std::mutex mutex_;
  ClockFn clock_;
  IrqFn irq_;
  uint32_t offset_ = 0;  // guest time minus host time, set by writes to LR
  uint32_t match_ = 0;
  bool int_mask_ = false, int_raw_ = false, irq_line_ = false;
};

}  // namespace kestrel
