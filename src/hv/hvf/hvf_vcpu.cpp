#include "hv/hvf/hvf_vcpu.h"

#include <mach/mach_time.h>

#include <format>

#include "arch/arm64/esr.h"
#include "arch/arm64/psci.h"
#include "core/log.h"
#include "hv/hvf/hvf_hypervisor.h"

namespace kestrel::hv {

using namespace kestrel::arm64;
namespace {

// SPSR/CPSR for EL1 with SP_EL1 (EL1h) and D, A, I, F masked.
constexpr uint64_t kCpsrEl1hMasked = 0x3c5;

std::chrono::nanoseconds ticks_to_ns(uint64_t ticks) {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    return t;
  }();
  return std::chrono::nanoseconds(ticks * tb.numer / tb.denom);
}

}  // namespace

Result<std::unique_ptr<HvfVcpu>> HvfVcpu::create(unsigned index) {
  hv_vcpu_t id;
  hv_vcpu_exit_t* exit = nullptr;
  if (auto r = hv_check(hv_vcpu_create(&id, &exit, nullptr), "hv_vcpu_create"); !r) {
    return std::unexpected(r.error());
  }
  auto vcpu = std::unique_ptr<HvfVcpu>(new HvfVcpu(id, exit));
  // GICv3 routes by affinity, and the device tree's cpu@N reg must match. Bit 31 is RES1.
  if (auto r = hv_check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_MPIDR_EL1, (1ull << 31) | index),
                        "set MPIDR_EL1");
      !r) {
    return std::unexpected(r.error());
  }
  return vcpu;
}

HvfVcpu::~HvfVcpu() { hv_vcpu_destroy(id_); }

void HvfVcpu::kick() { hv_vcpus_exit(&id_, 1); }

uint64_t HvfVcpu::reg(unsigned n) {
  if (n == 31) return 0;  // XZR
  uint64_t v = 0;
  hv_vcpu_get_reg(id_, static_cast<hv_reg_t>(HV_REG_X0 + n), &v);
  return v;
}

void HvfVcpu::set_reg(unsigned n, uint64_t value) {
  if (n == 31) return;  // writes to XZR are discarded
  hv_vcpu_set_reg(id_, static_cast<hv_reg_t>(HV_REG_X0 + n), value);
}

Result<void> HvfVcpu::reset_for_boot(uint64_t pc, uint64_t x0) {
  for (unsigned i = 0; i < 31; ++i) set_reg(i, 0);
  set_reg(0, x0);
  if (auto r = hv_check(hv_vcpu_set_reg(id_, HV_REG_CPSR, kCpsrEl1hMasked), "set CPSR"); !r) {
    return r;
  }
  return hv_check(hv_vcpu_set_reg(id_, HV_REG_PC, pc), "set PC");
}

Result<void> HvfVcpu::advance_pc() {
  uint64_t pc = 0;
  hv_vcpu_get_reg(id_, HV_REG_PC, &pc);
  return hv_check(hv_vcpu_set_reg(id_, HV_REG_PC, pc + 4), "advance PC");
}

std::string HvfVcpu::dump_state() {
  uint64_t pc = 0, cpsr = 0, far = 0, elr = 0, esr = 0;
  hv_vcpu_get_reg(id_, HV_REG_PC, &pc);
  hv_vcpu_get_reg(id_, HV_REG_CPSR, &cpsr);
  hv_vcpu_get_sys_reg(id_, HV_SYS_REG_FAR_EL1, &far);
  hv_vcpu_get_sys_reg(id_, HV_SYS_REG_ELR_EL1, &elr);
  hv_vcpu_get_sys_reg(id_, HV_SYS_REG_ESR_EL1, &esr);
  return std::format("pc={:#x} cpsr={:#x} elr_el1={:#x} esr_el1={:#x} far_el1={:#x} x0={:#x}",
                     pc, cpsr, elr, esr, far, reg(0));
}

Result<StopInfo> HvfVcpu::run(VcpuHost& host) {
  while (!host.stop_requested()) {
    if (auto r = hv_check(hv_vcpu_run(id_), "hv_vcpu_run"); !r) return std::unexpected(r.error());

    switch (exit_->reason) {
      case HV_EXIT_REASON_CANCELED:
        host.wait_while_paused();  // a pause kicks us here
        break;  // hv_vcpus_exit() from another thread; loop re-checks stop_requested()
      case HV_EXIT_REASON_VTIMER_ACTIVATED:
        // The in-kernel GIC delivers the timer PPI itself; just re-arm the exit.
        hv_vcpu_set_vtimer_mask(id_, false);
        break;
      case HV_EXIT_REASON_EXCEPTION: {
        auto stop = handle_exception(host);
        if (!stop) return std::unexpected(stop.error());
        if (*stop) return **stop;
        break;
      }
      default:
        return make_error(std::format("unexpected vCPU exit reason {}: {}",
                                      static_cast<int>(exit_->reason), dump_state()));
    }
  }
  return StopInfo{StopReason::kUserStop};
}

Result<std::optional<StopInfo>> HvfVcpu::handle_exception(VcpuHost& host) {
  const uint64_t esr = exit_->exception.syndrome;
  switch (static_cast<ExceptionClass>(exception_class(esr))) {
    case ExceptionClass::kDataAbortLower: {
      if (auto r = handle_mmio(host, esr, exit_->exception.physical_address); !r) {
        return std::unexpected(r.error());
      }
      return std::nullopt;
    }
    case ExceptionClass::kHvc64:
      return handle_psci(host);  // HVC's return address is already the next instruction
    case ExceptionClass::kSmc64: {
      auto stop = handle_psci(host);
      if (stop && !*stop) {
        if (auto r = advance_pc(); !r) return std::unexpected(r.error());
      }
      return stop;
    }
    case ExceptionClass::kWfiWfe:
      if ((esr & 1) == 0) handle_wfi(host);  // WFE (bit 0 set) just continues
      if (auto r = advance_pc(); !r) return std::unexpected(r.error());
      return std::nullopt;
    case ExceptionClass::kSysReg: {
      const SysRegAccess a = decode_sysreg(esr);
      log(LogLevel::kDebug, "unhandled sysreg {} op0={} op1={} crn={} crm={} op2={}",
          a.is_read ? "read" : "write", a.op0, a.op1, a.crn, a.crm, a.op2);
      if (a.is_read) set_reg(a.reg, 0);  // unknown registers read as zero
      if (auto r = advance_pc(); !r) return std::unexpected(r.error());
      return std::nullopt;
    }
    default:
      return make_error(std::format("unhandled guest exception EC={:#x} ESR={:#x}: {}",
                                    exception_class(esr), esr, dump_state()));
  }
}

Result<void> HvfVcpu::handle_mmio(VcpuHost& host, uint64_t esr, uint64_t ipa) {
  const auto abort = decode_data_abort(esr);
  if (!abort) {
    return make_error(std::format("MMIO access at {:#x} without instruction syndrome: {}", ipa,
                                  dump_state()));
  }
  const Gpa addr(ipa);
  if (abort->is_write) {
    uint64_t value = reg(abort->reg);
    if (abort->size_bytes < 8) value &= (1ull << (abort->size_bytes * 8)) - 1;
    if (!host.bus().write(addr, abort->size_bytes, value)) {
      log(LogLevel::kWarn, "guest wrote to unmapped address {:#x}", ipa);
    }
  } else {
    auto value = host.bus().read(addr, abort->size_bytes);
    if (!value) log(LogLevel::kWarn, "guest read from unmapped address {:#x}", ipa);
    uint64_t v = value.value_or(0);
    const unsigned bits = abort->size_bytes * 8;
    if (abort->sign_extend && bits < 64 && (v >> (bits - 1)) & 1) v |= ~0ull << bits;
    if (!abort->reg_is_64bit) v &= 0xffffffffull;
    set_reg(abort->reg, v);
  }
  return advance_pc();
}

Result<std::optional<StopInfo>> HvfVcpu::handle_psci(VcpuHost& host) {
  const PsciResult r = arm64::handle_psci(reg(0), reg(1));
  switch (r.action) {
    case PsciAction::kSystemOff:
      return StopInfo{StopReason::kPowerOff};
    case PsciAction::kSystemReset:
      return StopInfo{StopReason::kReset};
    case PsciAction::kIdle:
      handle_wfi(host);
      set_reg(0, static_cast<uint64_t>(r.value));
      return std::nullopt;
    case PsciAction::kCpuOn:
      set_reg(0, static_cast<uint64_t>(host.cpu_on(reg(1), reg(2), reg(3))));
      return std::nullopt;
    case PsciAction::kAffinityInfo:
      set_reg(0, static_cast<uint64_t>(host.affinity_info(reg(1))));
      return std::nullopt;
    case PsciAction::kCpuOff:
      return StopInfo{StopReason::kCpuOff};
    case PsciAction::kReturn:
      set_reg(0, static_cast<uint64_t>(r.value));
      return std::nullopt;
  }
  return std::nullopt;
}

void HvfVcpu::handle_wfi(VcpuHost& host) {
  // Sleep until the virtual timer's deadline, so an idle guest costs ~0% host CPU.
  constexpr auto kMaxIdle = std::chrono::milliseconds(50);
  uint64_t ctl = 0, cval = 0, offset = 0;
  hv_vcpu_get_sys_reg(id_, HV_SYS_REG_CNTV_CTL_EL0, &ctl);
  hv_vcpu_get_sys_reg(id_, HV_SYS_REG_CNTV_CVAL_EL0, &cval);
  hv_vcpu_get_vtimer_offset(id_, &offset);
  const bool timer_armed = (ctl & 1) && !(ctl & 2);  // ENABLE set, IMASK clear
  std::chrono::nanoseconds wait = kMaxIdle;
  if (timer_armed) {
    const uint64_t now = mach_absolute_time() - offset;
    if (cval <= now) return;  // already expired
    wait = std::min<std::chrono::nanoseconds>(ticks_to_ns(cval - now), kMaxIdle);
  }
  host.idle(wait);
}

}  // namespace kestrel::hv
