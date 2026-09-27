#pragma once

#include <cstdint>

namespace kestrel::arm64 {

// PSCI 1.0 (Arm DEN 0022) function IDs, SMC64/SMC32 calling convention.
namespace psci {
constexpr uint32_t kVersion = 0x84000000;
constexpr uint32_t kCpuSuspend32 = 0x84000001;
constexpr uint32_t kCpuSuspend64 = 0xC4000001;
constexpr uint32_t kCpuOff = 0x84000002;
constexpr uint32_t kCpuOn32 = 0x84000003;
constexpr uint32_t kCpuOn64 = 0xC4000003;
constexpr uint32_t kAffinityInfo32 = 0x84000004;
constexpr uint32_t kAffinityInfo64 = 0xC4000004;
constexpr uint32_t kMigrateInfoType = 0x84000006;
constexpr uint32_t kSystemOff = 0x84000008;
constexpr uint32_t kSystemReset = 0x84000009;
constexpr uint32_t kFeatures = 0x8400000A;

constexpr int64_t kSuccess = 0;
constexpr int64_t kNotSupported = -1;
constexpr int64_t kInvalidParameters = -2;
constexpr int64_t kAlreadyOn = -4;
constexpr uint32_t kVersion1_0 = 0x00010000;
constexpr int64_t kTosNotPresent = 2;  // MIGRATE_INFO_TYPE: no trusted OS
}  // namespace psci

enum class PsciAction {
  kReturn,       // put `value` in X0 and resume
  kIdle,         // CPU_SUSPEND: treat like WFI, then return success
  kCpuOn,        // start a secondary CPU: X1 = target MPIDR, X2 = entry, X3 = context
  kAffinityInfo, // report a CPU's power state: X1 = target MPIDR
  kCpuOff,       // stop this CPU
  kSystemOff,    // power down the VM
  kSystemReset,  // reboot the VM
};

struct PsciResult {
  PsciAction action;
  int64_t value;
};

// Pure decision logic; the vCPU loop performs the action.
PsciResult handle_psci(uint64_t function_id, uint64_t arg1);

}  // namespace kestrel::arm64
