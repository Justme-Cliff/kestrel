#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

#include "core/mmio_device.h"

namespace kestrel {

// Arm PrimeCell PL011 UART (DDI 0183). Serial console for bring-up.
// TX is instant (FIFO always empty); RX is fed by push_input().
class Pl011 final : public MmioDevice {
 public:
  using OutputFn = std::function<void(uint8_t)>;
  using IrqFn = std::function<void(bool level)>;

  Pl011(OutputFn output, IrqFn irq);

  std::string_view name() const override { return "pl011"; }
  uint64_t mmio_read(uint64_t offset, unsigned size) override;
  void mmio_write(uint64_t offset, unsigned size, uint64_t value) override;

  void push_input(uint8_t byte);

  // Interrupt bits (RIS/MIS/IMSC/ICR).
  static constexpr uint32_t kIntRx = 1u << 4;
  static constexpr uint32_t kIntTx = 1u << 5;
  static constexpr uint32_t kIntRxTimeout = 1u << 6;

 private:
  void update_irq_locked();

  static constexpr size_t kRxFifoLimit = 4096;

  std::mutex mutex_;
  OutputFn output_;
  IrqFn irq_;
  std::deque<uint8_t> rx_;
  uint32_t int_level_ = 0;
  uint32_t int_mask_ = 0;
  uint32_t ibrd_ = 0, fbrd_ = 0, lcr_h_ = 0, ifls_ = 0x12, dmacr_ = 0, ilpr_ = 0;
  uint32_t cr_ = 0x300;  // TXE | RXE
  bool irq_line_ = false;
};

}  // namespace kestrel
