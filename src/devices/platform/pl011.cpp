#include "devices/platform/pl011.h"

namespace kestrel {
namespace {

enum Reg : uint64_t {
  kDr = 0x000,
  kRsr = 0x004,
  kFr = 0x018,
  kIlpr = 0x020,
  kIbrd = 0x024,
  kFbrd = 0x028,
  kLcrH = 0x02c,
  kCr = 0x030,
  kIfls = 0x034,
  kImsc = 0x038,
  kRis = 0x03c,
  kMis = 0x040,
  kIcr = 0x044,
  kDmacr = 0x048,
  kPeriphId0 = 0xfe0,
};

constexpr uint32_t kFrRxfe = 1u << 4;
constexpr uint32_t kFrTxfe = 1u << 7;
constexpr uint32_t kIntAll = 0x7ff;

// PeriphID0-3 then PCellID0-3: identifies a PL011 to the Linux AMBA bus.
constexpr uint8_t kIds[8] = {0x11, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1};

}  // namespace

Pl011::Pl011(OutputFn output, IrqFn irq) : output_(std::move(output)), irq_(std::move(irq)) {}

void Pl011::update_irq_locked() {
  const bool level = (int_level_ & int_mask_) != 0;
  if (level != irq_line_) {
    irq_line_ = level;
    irq_(level);
  }
}

uint64_t Pl011::mmio_read(uint64_t offset, unsigned /*size*/) {
  std::lock_guard lock(mutex_);
  switch (offset) {
    case kDr: {
      if (rx_.empty()) return 0;
      const uint8_t c = rx_.front();
      rx_.pop_front();
      if (rx_.empty()) int_level_ &= ~(kIntRx | kIntRxTimeout);
      update_irq_locked();
      return c;
    }
    case kRsr: return 0;
    case kFr: return kFrTxfe | (rx_.empty() ? kFrRxfe : 0);
    case kIlpr: return ilpr_;
    case kIbrd: return ibrd_;
    case kFbrd: return fbrd_;
    case kLcrH: return lcr_h_;
    case kCr: return cr_;
    case kIfls: return ifls_;
    case kImsc: return int_mask_;
    case kRis: return int_level_;
    case kMis: return int_level_ & int_mask_;
    case kDmacr: return dmacr_;
    default:
      if (offset >= kPeriphId0 && offset < kPeriphId0 + 32 && offset % 4 == 0) {
        return kIds[(offset - kPeriphId0) / 4];
      }
      return 0;
  }
}

void Pl011::mmio_write(uint64_t offset, unsigned /*size*/, uint64_t value) {
  std::lock_guard lock(mutex_);
  const auto v = static_cast<uint32_t>(value);
  switch (offset) {
    case kDr:
      output_(static_cast<uint8_t>(v));
      int_level_ |= kIntTx;  // FIFO drained immediately
      break;
    case kRsr: break;
    case kIlpr: ilpr_ = v; break;
    case kIbrd: ibrd_ = v; break;
    case kFbrd: fbrd_ = v; break;
    case kLcrH: lcr_h_ = v; break;
    case kCr: cr_ = v; break;
    case kIfls: ifls_ = v; break;
    case kImsc: int_mask_ = v & kIntAll; break;
    case kIcr: int_level_ &= ~v; break;
    case kDmacr: dmacr_ = v; break;
    default: break;
  }
  update_irq_locked();
}

void Pl011::push_input(uint8_t byte) {
  std::lock_guard lock(mutex_);
  if (rx_.size() >= kRxFifoLimit) return;  // drop: guest isn't reading
  rx_.push_back(byte);
  int_level_ |= kIntRx;
  update_irq_locked();
}

}  // namespace kestrel
