#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/guest_memory.h"
#include "core/mmio_bus.h"
#include "hv/hypervisor.h"

namespace kestrel {

struct MachineConfig {
  uint64_t ram_size = 1024ull << 20;
  unsigned cpus = 1;
};

// The whole virtual machine: RAM, interrupt controller, devices and vCPUs.
// Host-independent: all hypervisor access goes through hv::Hypervisor.
class Machine final : public InterruptController, public hv::VcpuHost {
 public:
  static constexpr unsigned kMaxCpus = 8;

  static Result<std::unique_ptr<Machine>> create(const MachineConfig& config);
  ~Machine() override;

  GuestMemory& memory() { return memory_; }
  const hv::GicInfo& gic() const { return gic_; }
  const MachineConfig& config() const { return config_; }

  // Takes ownership of `device` and maps it at [base, base+size).
  Result<void> attach(Gpa base, uint64_t size, std::unique_ptr<MmioDevice> device);
  // Maps a device owned elsewhere (it must outlive the machine's vCPUs).
  Result<void> attach_borrowed(Gpa base, uint64_t size, MmioDevice& device);
  // Keeps a non-MMIO object (e.g. a device's worker) alive as long as the machine.
  template <typename T>
  T* own(std::unique_ptr<T> object) {
    T* raw = object.get();
    owned_.push_back(std::shared_ptr<void>(std::move(object)));
    return raw;
  }

  // Maps host memory into the guest physical address space at runtime (thread-safe).
  Result<void> map_memory(void* host, uint64_t gpa, uint64_t size, bool writable) {
    return hv_->map_memory(host, gpa, size, writable);
  }
  Result<void> unmap_memory(uint64_t gpa, uint64_t size) { return hv_->unmap_memory(gpa, size); }

  // Boots CPU 0 at `pc` with X0 = `x0` and blocks until the guest stops.
  Result<hv::StopInfo> run(uint64_t pc, uint64_t x0);

  // Thread-safe: asks all vCPUs to stop.
  void request_stop();
  // Stops (true) or resumes (false) every guest CPU. Devices keep running; the
  // guest's clock jumps forward on resume, like a laptop waking from sleep.
  void set_paused(bool paused);
  bool paused() const { return paused_.load(); }

  // InterruptController
  void set_spi(uint32_t spi, bool level) override;

  // hv::VcpuHost
  const MmioBus& bus() const override { return bus_; }
  bool stop_requested() const override { return stop_.load(); }
  void idle(std::chrono::nanoseconds max_wait) override;
  int64_t cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context) override;
  int64_t affinity_info(uint64_t mpidr) override;
  void wait_while_paused() override;

 private:
  enum class CpuState { kOff, kPending, kOn };
  struct Cpu {
    CpuState state = CpuState::kOff;
    uint64_t entry = 0, context = 0;
    hv::Vcpu* vcpu = nullptr;  // valid while its thread holds it
  };

  Machine(MachineConfig config, std::unique_ptr<hv::Hypervisor> hv, GuestMemory memory,
          hv::GicInfo gic)
      : config_(config), hv_(std::move(hv)), memory_(std::move(memory)), gic_(gic) {}

  void cpu_thread(unsigned index);
  void finish(Result<hv::StopInfo> result);
  void wake();

  MachineConfig config_;
  std::unique_ptr<hv::Hypervisor> hv_;
  GuestMemory memory_;
  hv::GicInfo gic_;
  MmioBus bus_;
  std::vector<std::unique_ptr<MmioDevice>> devices_;
  // Destroyed before devices_ (reverse declaration order).
  std::vector<std::shared_ptr<void>> owned_;

  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
  std::mutex pause_mutex_;
  std::condition_variable pause_cv_;
  std::atomic<bool> shutting_down_{false};
  std::mutex cpu_mutex_;
  std::condition_variable cpu_cv_;
  std::vector<Cpu> cpus_;
  unsigned cpus_created_ = 0;
  std::optional<Result<hv::StopInfo>> result_;

  std::mutex idle_mutex_;
  std::condition_variable idle_cv_;
  uint64_t wake_generation_ = 0;
};

}  // namespace kestrel
