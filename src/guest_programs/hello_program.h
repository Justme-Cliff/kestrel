#pragma once

#include <cstdint>
#include <span>

namespace kestrel {

// A tiny position-independent arm64 program (Phase 1) that prints a greeting on
// the PL011 UART at 0x09000000 and powers off through PSCI SYSTEM_OFF.
std::span<const uint8_t> hello_program();

}  // namespace kestrel
