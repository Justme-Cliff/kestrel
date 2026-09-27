#pragma once

#include <Hypervisor/Hypervisor.h>

#include <memory>
#include <optional>
#include <string>

#include "hv/hypervisor.h"

namespace kestrel::hv {

// Hypervisor.framework vCPU. Exits are decoded here using the shared arm64 logic
// (ESR decoding, PSCI) from src/arch/arm64.
class HvfVcpu final : public Vcpu {
 public:
  static Result<std::unique_ptr<HvfVcpu>> create(unsigned index);
  ~HvfVcpu() override;

  Result<void> reset_for_boot(uint64_t pc, uint64_t x0) override;
  Result<StopInfo> run(VcpuHost& host) override;
  void kick() override;

 private:
  HvfVcpu(hv_vcpu_t id, hv_vcpu_exit_t* exit) : id_(id), exit_(exit) {}

  // Returns a StopInfo when the guest asked to stop, nullopt to keep running.
  Result<std::optional<StopInfo>> handle_exception(VcpuHost& host);
  Result<void> handle_mmio(VcpuHost& host, uint64_t esr, uint64_t ipa);
  Result<std::optional<StopInfo>> handle_psci(VcpuHost& host);
  void handle_wfi(VcpuHost& host);
  Result<void> advance_pc();

  uint64_t reg(unsigned n);
  void set_reg(unsigned n, uint64_t value);
  std::string dump_state();

  hv_vcpu_t id_;
  hv_vcpu_exit_t* exit_;
};

}  // namespace kestrel::hv
