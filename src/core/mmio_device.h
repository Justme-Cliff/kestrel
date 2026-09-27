#pragma once

#include <cstdint>
#include <string_view>

namespace kestrel {

// A device with a memory-mapped register window. Called from vCPU threads,
// so implementations must be thread-safe.
class MmioDevice {
 public:
  virtual ~MmioDevice() = default;
  virtual std::string_view name() const = 0;
  virtual uint64_t mmio_read(uint64_t offset, unsigned size) = 0;
  virtual void mmio_write(uint64_t offset, unsigned size, uint64_t value) = 0;
};

// One interrupt line into the interrupt controller.
class InterruptController {
 public:
  virtual ~InterruptController() = default;
  // `spi` is the SPI number (0-based, as written in the device tree), not the INTID.
  virtual void set_spi(uint32_t spi, bool level) = 0;
};

}  // namespace kestrel
