#include "arch/arm64/esr.h"

namespace kestrel::arm64 {

std::optional<DataAbort> decode_data_abort(uint64_t esr) {
  if (exception_class(esr) != static_cast<uint8_t>(ExceptionClass::kDataAbortLower)) {
    return std::nullopt;
  }
  const bool isv = (esr >> 24) & 1;
  if (!isv) return std::nullopt;
  return DataAbort{
      .is_write = ((esr >> 6) & 1) != 0,
      .size_bytes = 1u << ((esr >> 22) & 3),
      .reg = static_cast<unsigned>((esr >> 16) & 0x1f),
      .sign_extend = ((esr >> 21) & 1) != 0,
      .reg_is_64bit = ((esr >> 15) & 1) != 0,
  };
}

SysRegAccess decode_sysreg(uint64_t esr) {
  return SysRegAccess{
      .is_read = (esr & 1) != 0,
      .reg = static_cast<unsigned>((esr >> 5) & 0x1f),
      .op0 = static_cast<unsigned>((esr >> 20) & 3),
      .op1 = static_cast<unsigned>((esr >> 14) & 7),
      .crn = static_cast<unsigned>((esr >> 10) & 0xf),
      .crm = static_cast<unsigned>((esr >> 1) & 0xf),
      .op2 = static_cast<unsigned>((esr >> 17) & 7),
  };
}

}  // namespace kestrel::arm64
