#include "core/machine.h"
#include "core/thread_priority.h"

#include <format>

#include "arch/arm64/psci.h"
#include "arch/arm64/virt_layout.h"
#include "core/log.h"

namespace kestrel {

namespace psci = arm64::psci;

Result<std::unique_ptr<Machine>> Machine::create(const MachineConfig& config) {
  if (config.cpus == 0 || config.cpus > kMaxCpus) {
    return make_error(std::format("CPU count must be 1-{}", kMaxCpus));
  }
  auto hv = hv::create_hypervisor();
  if (!hv) return std::unexpected(hv.error());
  auto memory = GuestMemory::create(Gpa(arm64::layout::kRamBase), config.ram_size);
  if (!memory) return std::unexpected(memory.error());
  if (auto r = (*hv)->map_ram(*memory); !r) return std::unexpected(r.error());
  auto gic = (*hv)->create_gic(config.cpus);
  if (!gic) return std::unexpected(gic.error());
  return std::unique_ptr<Machine>(new Machine(config, std::move(*hv), std::move(*memory), *gic));
}

Machine::~Machine() {
  request_stop();
  // Devices and helpers run their own threads (disk I/O, networking, GPU fences)
  // that can still raise interrupts. Stop them while every other member — the
  // idle lock, the interrupt controller — is still alive.
  shutting_down_ = true;
  owned_.clear();
  devices_.clear();
}

Result<void> Machine::attach(Gpa base, uint64_t size, std::unique_ptr<MmioDevice> device) {
  if (!bus_.add(base, size, device.get())) {
    return make_error(std::format("cannot map {} at {:#x}: overlaps another device",
                                  device->name(), base.value));
  }
  devices_.push_back(std::move(device));
  return {};
}

Result<void> Machine::attach_borrowed(Gpa base, uint64_t size, MmioDevice& device) {
  if (!bus_.add(base, size, &device)) {
    return make_error(std::format("cannot map {} at {:#x}: overlaps another device", device.name(),
                                  base.value));
  }
  return {};
}

Result<hv::StopInfo> Machine::run(uint64_t pc, uint64_t x0) {
  // A stop requested while the machine was being set up (e.g. Ctrl-C or quitting
  // during startup) must be honored, so stop_ is never reset here.
  if (stop_) return hv::StopInfo{hv::StopReason::kUserStop};
  {
    std::lock_guard lock(cpu_mutex_);
    cpus_.assign(config_.cpus, Cpu{});
    cpus_[0] = Cpu{.state = CpuState::kPending, .entry = pc, .context = x0};
    cpus_created_ = 0;
    result_.reset();
  }
  // One host thread per vCPU: backends bind a vCPU to its creating thread.
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < config_.cpus; ++i) threads.emplace_back([this, i] { cpu_thread(i); });
  for (auto& t : threads) t.join();
  std::lock_guard lock(cpu_mutex_);
  return result_.value_or(hv::StopInfo{hv::StopReason::kUserStop});
}

void Machine::finish(Result<hv::StopInfo> result) {
  {
    std::lock_guard lock(cpu_mutex_);
    if (!result_) result_ = std::move(result);
  }
  request_stop();
}

void Machine::cpu_thread(unsigned index) {
  set_thread_role(ThreadRole::kInteractive);
  auto vcpu = hv_->create_vcpu(index);
  std::unique_lock lock(cpu_mutex_);
  ++cpus_created_;
  cpu_cv_.notify_all();
  if (!vcpu) {
    lock.unlock();
    finish(std::unexpected(vcpu.error()));
    return;
  }
  cpus_[index].vcpu = vcpu->get();
  // The in-kernel GIC fixes the topology once any vCPU runs, so wait for all of them.
  cpu_cv_.wait(lock, [&] { return cpus_created_ == config_.cpus || stop_; });

  while (true) {
    cpu_cv_.wait(lock, [&] { return cpus_[index].state == CpuState::kPending || stop_; });
    if (stop_) break;
    const uint64_t entry = cpus_[index].entry, context = cpus_[index].context;
    cpus_[index].state = CpuState::kOn;
    lock.unlock();

    Result<hv::StopInfo> r = (*vcpu)->reset_for_boot(entry, context).and_then(
        [&] { return (*vcpu)->run(*this); });

    lock.lock();
    if (r && r->reason == hv::StopReason::kCpuOff) {
      cpus_[index].state = CpuState::kOff;
      continue;
    }
    lock.unlock();
    finish(std::move(r));
    lock.lock();
    break;
  }
  cpus_[index].vcpu = nullptr;
  lock.unlock();
  vcpu->reset();  // destroy on the creating thread
}

int64_t Machine::cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context) {
  const uint64_t index = mpidr & 0xffffff;
  std::lock_guard lock(cpu_mutex_);
  if (index >= cpus_.size()) return psci::kInvalidParameters;
  if (cpus_[index].state != CpuState::kOff) return psci::kAlreadyOn;
  cpus_[index] = Cpu{.state = CpuState::kPending, .entry = entry, .context = context,
                     .vcpu = cpus_[index].vcpu};
  cpu_cv_.notify_all();
  return psci::kSuccess;
}

int64_t Machine::affinity_info(uint64_t mpidr) {
  const uint64_t index = mpidr & 0xffffff;
  std::lock_guard lock(cpu_mutex_);
  if (index >= cpus_.size()) return psci::kInvalidParameters;
  switch (cpus_[index].state) {
    case CpuState::kOn: return 0;
    case CpuState::kOff: return 1;
    case CpuState::kPending: return 2;
  }
  return 1;
}

void Machine::set_paused(bool paused) {
  {
    std::lock_guard lock(pause_mutex_);
    if (paused_ == paused) return;
    paused_ = paused;
  }
  pause_cv_.notify_all();
  if (paused) {
    std::lock_guard lock(cpu_mutex_);
    for (auto& cpu : cpus_) {
      if (cpu.vcpu) cpu.vcpu->kick();
    }
  }
}

void Machine::wait_while_paused() {
  std::unique_lock lock(pause_mutex_);
  pause_cv_.wait(lock, [&] { return !paused_ || stop_; });
}

void Machine::request_stop() {
  stop_ = true;
  {
    std::lock_guard lock(pause_mutex_);
  }
  pause_cv_.notify_all();
  {
    std::lock_guard lock(cpu_mutex_);
    for (auto& cpu : cpus_) {
      if (cpu.vcpu) cpu.vcpu->kick();
    }
  }
  cpu_cv_.notify_all();
  wake();
}

void Machine::set_spi(uint32_t spi, bool level) {
  if (shutting_down_) return;
  if (spi >= gic_.spi_count) {
    log(LogLevel::kError, "SPI {} out of range", spi);
    return;
  }
  hv_->set_spi_intid(gic_.spi_intid_base + spi, level);
  if (level) wake();
}

void Machine::wake() {
  {
    std::lock_guard lock(idle_mutex_);
    ++wake_generation_;
  }
  idle_cv_.notify_all();
}

void Machine::idle(std::chrono::nanoseconds max_wait) {
  std::unique_lock lock(idle_mutex_);
  const uint64_t gen = wake_generation_;
  idle_cv_.wait_for(lock, max_wait, [&] { return wake_generation_ != gen || stop_.load(); });
}

}  // namespace kestrel
