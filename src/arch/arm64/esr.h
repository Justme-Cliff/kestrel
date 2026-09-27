#pragma once

#include <cstdint>
#include <optional>

namespace kestrel::arm64 {

// Exception classes (ESR_ELx.EC), Arm ARM D23.2.41.
enum class ExceptionClass : uint8_t {
  kWfiWfe = 0x01,
  kHvc64 = 0x16,
  kSmc64 = 0x17,
  kSysReg = 0x18,
  kInstructionAbortLower = 0x20,
  kDataAbortLower = 0x24,
};

constexpr uint8_t exception_class(uint64_t esr) { return static_cast<uint8_t>((esr >> 26) & 0x3f); }

// A decoded data abort with a valid instruction syndrome (ISV=1).
struct DataAbort {
  bool is_write;
  unsigned size_bytes;  // 1, 2, 4 or 8
  unsigned reg;         // 0-30, or 31 for XZR
  bool sign_extend;
  bool reg_is_64bit;
};

// Returns nullopt if `esr` isn't a data abort or has no valid syndrome (ISV=0).
std::optional<DataAbort> decode_data_abort(uint64_t esr);

// A trapped MSR/MRS (EC 0x18).
struct SysRegAccess {
  bool is_read;
  unsigned reg;  // Rt
  unsigned op0, op1, crn, crm, op2;
};

SysRegAccess decode_sysreg(uint64_t esr);

}  // namespace kestrel::arm64
