#include "arch/arm64/psci.h"

namespace kestrel::arm64 {

PsciResult handle_psci(uint64_t function_id, uint64_t arg1) {
  using namespace psci;
  switch (static_cast<uint32_t>(function_id)) {
    case kVersion:
      return {PsciAction::kReturn, kVersion1_0};
    case kCpuSuspend32:
    case kCpuSuspend64:
      return {PsciAction::kIdle, kSuccess};
    case kCpuOff:
      return {PsciAction::kCpuOff, kSuccess};
    case kCpuOn32:
    case kCpuOn64:
      return {PsciAction::kCpuOn, kSuccess};
    case kAffinityInfo32:
    case kAffinityInfo64:
      return {PsciAction::kAffinityInfo, 0};
    case kMigrateInfoType:
      return {PsciAction::kReturn, kTosNotPresent};
    case kSystemOff:
      return {PsciAction::kSystemOff, kSuccess};
    case kSystemReset:
      return {PsciAction::kSystemReset, kSuccess};
    case kFeatures:
      switch (static_cast<uint32_t>(arg1)) {
        case kVersion: case kCpuSuspend32: case kCpuSuspend64: case kCpuOff: case kCpuOn32:
        case kCpuOn64: case kAffinityInfo32: case kAffinityInfo64: case kMigrateInfoType:
        case kSystemOff: case kSystemReset: case kFeatures:
          return {PsciAction::kReturn, kSuccess};
        default:
          return {PsciAction::kReturn, kNotSupported};
      }
    default:
      return {PsciAction::kReturn, kNotSupported};
  }
}

}  // namespace kestrel::arm64
