#include "hv/hvf/hvf_hypervisor.h"

#include <format>

#include "arch/arm64/virt_layout.h"
#include "core/log.h"
#include "hv/hvf/hvf_vcpu.h"

namespace kestrel::hv {

Result<void> hv_check(hv_return_t rc, std::string_view what) {
  if (rc == HV_SUCCESS) return {};
  std::string hint;
  if (rc == HV_DENIED) hint = " (binary not signed with com.apple.security.hypervisor?)";
  if (rc == HV_BUSY) hint = " (another VM already exists in this process?)";
  return make_error(std::format("{} failed: hv error {:#x}{}", what, static_cast<uint32_t>(rc),
                                hint));
}

Result<std::unique_ptr<HvfHypervisor>> HvfHypervisor::create() {
  hv_vm_config_t config = hv_vm_config_create();
  uint32_t ipa_bits = 0;
  hv_vm_config_get_default_ipa_size(&ipa_bits);
  log(LogLevel::kDebug, "default IPA size: {} bits", ipa_bits);
  const hv_return_t rc = hv_vm_create(config);
  os_release(config);
  if (auto r = hv_check(rc, "hv_vm_create"); !r) return std::unexpected(r.error());
  return std::unique_ptr<HvfHypervisor>(new HvfHypervisor());
}

HvfHypervisor::~HvfHypervisor() { hv_vm_destroy(); }

Result<void> HvfHypervisor::map_ram(const GuestMemory& memory) {
  return hv_check(hv_vm_map(memory.host_base(), memory.base().value, memory.size(),
                            HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
                  "hv_vm_map");
}

Result<GicInfo> HvfHypervisor::create_gic(unsigned cpus) {
  namespace L = arm64::layout;
  size_t dist_size = 0, dist_align = 0, redist_region = 0, redist_size = 0, redist_align = 0;
  hv_gic_get_distributor_size(&dist_size);
  hv_gic_get_distributor_base_alignment(&dist_align);
  hv_gic_get_redistributor_region_size(&redist_region);
  hv_gic_get_redistributor_size(&redist_size);
  hv_gic_get_redistributor_base_alignment(&redist_align);
  log(LogLevel::kDebug, "GIC: dist {:#x} (align {:#x}), redist {:#x}/cpu region {:#x} (align {:#x})",
      dist_size, dist_align, redist_size, redist_region, redist_align);

  GicInfo info{};
  info.dist_base = L::kGicDistBase;
  info.dist_size = dist_size;
  info.redist_base = L::kGicRedistBase;
  info.redist_size = redist_size * cpus;
  if (info.dist_base % std::max<size_t>(dist_align, 1) != 0 ||
      info.redist_base % std::max<size_t>(redist_align, 1) != 0 ||
      info.dist_base + dist_size > L::kUartBase ||
      info.redist_base + redist_region > L::kGicRedistLimit) {
    return make_error(std::format("GIC regions don't fit the memory map (redist region {:#x})",
                                  redist_region));
  }

  hv_gic_config_t config = hv_gic_config_create();
  hv_gic_config_set_distributor_base(config, info.dist_base);
  hv_gic_config_set_redistributor_base(config, info.redist_base);
  const hv_return_t rc = hv_gic_create(config);
  os_release(config);
  if (auto r = hv_check(rc, "hv_gic_create"); !r) return std::unexpected(r.error());

  hv_gic_get_spi_interrupt_range(&info.spi_intid_base, &info.spi_count);
  return info;
}

}  // namespace kestrel::hv

namespace kestrel::hv {

Result<void> HvfHypervisor::map_memory(void* host, uint64_t gpa, uint64_t size, bool writable) {
  hv_memory_flags_t flags = HV_MEMORY_READ | (writable ? HV_MEMORY_WRITE : 0);
  return hv_check(hv_vm_map(host, gpa, size, flags), "hv_vm_map (device memory)");
}

Result<void> HvfHypervisor::unmap_memory(uint64_t gpa, uint64_t size) {
  return hv_check(hv_vm_unmap(gpa, size), "hv_vm_unmap");
}

void HvfHypervisor::set_spi_intid(uint32_t intid, bool level) { hv_gic_set_spi(intid, level); }

Result<std::unique_ptr<Vcpu>> HvfHypervisor::create_vcpu(unsigned index) {
  auto vcpu = HvfVcpu::create(index);
  if (!vcpu) return std::unexpected(vcpu.error());
  return std::unique_ptr<Vcpu>(std::move(*vcpu));
}

Result<std::unique_ptr<Hypervisor>> create_hypervisor() {
  auto hv = HvfHypervisor::create();
  if (!hv) return std::unexpected(hv.error());
  return std::unique_ptr<Hypervisor>(std::move(*hv));
}

}  // namespace kestrel::hv
