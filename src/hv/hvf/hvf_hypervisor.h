#pragma once

#include <Hypervisor/Hypervisor.h>

#include <string_view>

#include "hv/hypervisor.h"

namespace kestrel::hv {

Result<void> hv_check(hv_return_t rc, std::string_view what);

// macOS Hypervisor.framework backend (Apple Silicon).
class HvfHypervisor final : public Hypervisor {
 public:
  static Result<std::unique_ptr<HvfHypervisor>> create();
  ~HvfHypervisor() override;

  Result<void> map_ram(const GuestMemory& memory) override;
  Result<void> map_memory(void* host, uint64_t gpa, uint64_t size, bool writable) override;
  Result<void> unmap_memory(uint64_t gpa, uint64_t size) override;
  Result<GicInfo> create_gic(unsigned cpus) override;
  void set_spi_intid(uint32_t intid, bool level) override;
  Result<std::unique_ptr<Vcpu>> create_vcpu(unsigned index) override;

 private:
  HvfHypervisor() = default;
};

}  // namespace kestrel::hv
