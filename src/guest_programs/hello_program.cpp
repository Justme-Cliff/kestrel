#include "guest_programs/hello_program.h"

// The guest code lives in the host binary; both are arm64, so the assembler
// encodes it for us. It is copied into guest RAM and never run on the host.
asm(R"(
  .section __TEXT,__const
  .p2align 2
  .globl _kestrel_hello_start
  .globl _kestrel_hello_end
_kestrel_hello_start:
  movz x1, #0x0900, lsl #16      // x1 = PL011 UART data register
  adr  x2, 3f                    // x2 = message
1:
  ldrb w3, [x2], #1
  cbz  w3, 2f
  strb w3, [x1]                  // MMIO write -> VM exit -> Pl011::mmio_write
  b    1b
2:
  movz x0, #0x0008
  movk x0, #0x8400, lsl #16      // PSCI SYSTEM_OFF
  hvc  #0
  b    .
3:
  .asciz "hello from inside the Kestrel VM!\n"
  .p2align 2
_kestrel_hello_end:
  .text
)");

extern "C" const uint8_t kestrel_hello_start[];
extern "C" const uint8_t kestrel_hello_end[];

namespace kestrel {

std::span<const uint8_t> hello_program() {
  return {kestrel_hello_start, static_cast<size_t>(kestrel_hello_end - kestrel_hello_start)};
}

}  // namespace kestrel
