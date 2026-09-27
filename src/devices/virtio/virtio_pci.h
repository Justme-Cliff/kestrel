#pragma once

#include <memory>

#include "devices/pci/pci.h"
#include "devices/virtio/virtio_transport.h"

namespace kestrel {

// Modern (virtio 1.x only) virtio-pci transport (virtio 1.2 §4.1), INTx interrupts.
// BAR0 layout: common config 0x0000, ISR 0x1000, device config 0x2000, notify 0x3000.
class VirtioPci final : public PciFunction {
 public:
  VirtioPci(const GuestMemory& memory, std::unique_ptr<VirtioDevice> device);

  std::string_view name() const override { return transport_.device().name(); }
  uint64_t bar_read(unsigned bar, uint64_t offset, unsigned size) override;
  void bar_write(unsigned bar, uint64_t offset, unsigned size, uint64_t value) override;

  static constexpr uint64_t kCommonOffset = 0x0000;
  static constexpr uint64_t kIsrOffset = 0x1000;
  static constexpr uint64_t kDeviceOffset = 0x2000;
  static constexpr uint64_t kNotifyOffset = 0x3000;
  static constexpr uint32_t kNotifyMultiplier = 4;
  static constexpr uint64_t kBarSize = 0x4000;

 private:
  uint64_t common_read(uint64_t offset, unsigned size);
  void common_write(uint64_t offset, unsigned size, uint32_t value);

  mutable VirtioTransport transport_;
  uint32_t device_features_sel_ = 0, driver_features_sel_ = 0;
};

}  // namespace kestrel
