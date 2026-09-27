#pragma once

#include <memory>

#include "core/mmio_device.h"
#include "devices/virtio/virtio_transport.h"

namespace kestrel {

// virtio-mmio transport, version 2 (virtio 1.2 §4.2).
class VirtioMmio final : public MmioDevice {
 public:
  VirtioMmio(const GuestMemory& memory, std::unique_ptr<VirtioDevice> device,
             VirtioTransport::IrqFn irq)
      : transport_(memory, std::move(device), std::move(irq)) {}

  std::string_view name() const override { return transport_.device().name(); }
  uint64_t mmio_read(uint64_t offset, unsigned size) override;
  void mmio_write(uint64_t offset, unsigned size, uint64_t value) override;

 private:
  // mutable: device() is non-const but name() must be const.
  mutable VirtioTransport transport_;
  uint32_t device_features_sel_ = 0, driver_features_sel_ = 0;
};

}  // namespace kestrel
