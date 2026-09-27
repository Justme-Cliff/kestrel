#include "devices/platform/pl031.h"

namespace kestrel {
namespace {

enum Reg : uint64_t {
  kDr = 0x00,
  kMr = 0x04,
  kLr = 0x08,
  kCr = 0x0c,
  kImsc = 0x10,
  kRis = 0x14,
  kMis = 0x18,
  kIcr = 0x1c,
  kPeriphId0 = 0xfe0,
};

// PeriphID0-3 then PCellID0-3: identifies a PL031 to the Linux AMBA bus.
constexpr uint8_t kIds[8] = {0x31, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1};

}  // namespace

Pl031::Pl031(ClockFn clock, IrqFn irq) : clock_(std::move(clock)), irq_(std::move(irq)) {}

void Pl031::update_irq_locked() {
  const bool level = int_raw_ && int_mask_;
  if (level != irq_line_) {
    irq_line_ = level;
    irq_(level);
  }
}

uint64_t Pl031::mmio_read(uint64_t offset, unsigned /*size*/) {
  std::lock_guard lock(mutex_);
  switch (offset) {
    case kDr: return now_locked();
    case kMr: return match_;
    case kLr: return now_locked();
    case kCr: return 1;  // always running
    case kImsc: return int_mask_;
    case kRis: return int_raw_;
    case kMis: return int_raw_ && int_mask_;
    default:
      if (offset >= kPeriphId0 && offset < kPeriphId0 + 32 && offset % 4 == 0) {
        return kIds[(offset - kPeriphId0) / 4];
      }
      return 0;
  }
}

void Pl031::mmio_write(uint64_t offset, unsigned /*size*/, uint64_t value) {
  std::lock_guard lock(mutex_);
  const auto v = static_cast<uint32_t>(value);
  switch (offset) {
    case kMr:
      // Alarms aren't needed yet: the match interrupt fires only if already due.
      match_ = v;
      if (match_ == now_locked()) int_raw_ = true;
      break;
    case kLr: offset_ = v - clock_(); break;
    case kImsc: int_mask_ = v & 1; break;
    case kIcr:
      if (v & 1) int_raw_ = false;
      break;
    default: break;
  }
  update_irq_locked();
}

}  // namespace kestrel
