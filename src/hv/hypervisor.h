#pragma once

// Host hypervisor abstraction (docs/DESIGN.md §1.2). Only src/hv/<backend>/ may include
// OS hypervisor headers; the rest of Kestrel talks to these interfaces.
// Backends: hvf (macOS, now), kvm (Linux, Phase 11), whp (Windows, Phase 12).

#include <chrono>
#include <cstdint>
#include <memory>

#include "core/error.h"
#include "core/guest_memory.h"
#include "core/mmio_bus.h"

namespace kestrel::hv {

enum class StopReason { kPowerOff, kReset, kUserStop, kCpuOff };

struct StopInfo {
  StopReason reason;
};

struct GicInfo {
  uint64_t dist_base, dist_size;
  uint64_t redist_base, redist_size;  // redist_size covers all vCPUs
  uint32_t spi_intid_base, spi_count;
};

// What a vCPU needs from the machine that owns it.
class VcpuHost {
 public:
  virtual ~VcpuHost() = default;
  virtual const MmioBus& bus() const = 0;
  virtual bool stop_requested() const = 0;
  // Sleep until woken by an interrupt source or `max_wait` passes.
  virtual void idle(std::chrono::nanoseconds max_wait) = 0;
  // PSCI CPU_ON / AFFINITY_INFO; return PSCI status codes.
  virtual int64_t cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context) = 0;
  virtual int64_t affinity_info(uint64_t mpidr) = 0;
  // Called after a kick: blocks while the VM is paused.
  virtual void wait_while_paused() {}
};

// A guest CPU. Backends may tie a vCPU to its creating thread, so create, run and
// destroy it on one thread. kick() is the only method safe to call from elsewhere.
class Vcpu {
 public:
  virtual ~Vcpu() = default;
  // arm64 Linux boot state: EL1h, interrupts masked, MMU off, PC = pc, X0 = x0.
  virtual Result<void> reset_for_boot(uint64_t pc, uint64_t x0) = 0;
  // Runs until power off, reset, CPU_OFF, a stop request, or an error.
  virtual Result<StopInfo> run(VcpuHost& host) = 0;
  // Forces run() to return to its loop promptly (thread-safe).
  virtual void kick() = 0;
};

// One VM on the host hypervisor. Most hosts allow one per process.
class Hypervisor {
 public:
  virtual ~Hypervisor() = default;
  virtual Result<void> map_ram(const GuestMemory& memory) = 0;
  // Maps host memory (e.g. a GPU buffer) at a guest physical address while the VM
  // runs. Address and size must be host-page aligned (16 KB on Apple Silicon).
  virtual Result<void> map_memory(void* host, uint64_t gpa, uint64_t size, bool writable) = 0;
  virtual Result<void> unmap_memory(uint64_t gpa, uint64_t size) = 0;
  // Must be called before any vCPU is created.
  virtual Result<GicInfo> create_gic(unsigned cpus) = 0;
  virtual void set_spi_intid(uint32_t intid, bool level) = 0;
  // Creates vCPU `index` (MPIDR affinity = index) on the calling thread.
  virtual Result<std::unique_ptr<Vcpu>> create_vcpu(unsigned index) = 0;
};

// Creates the backend for the current host OS.
Result<std::unique_ptr<Hypervisor>> create_hypervisor();

}  // namespace kestrel::hv
