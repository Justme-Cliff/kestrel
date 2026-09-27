// Runs real guest code under Hypervisor.framework. The binary must be signed
// with the hypervisor entitlement (CMake does this).

#include <gtest/gtest.h>

#include <string>

#include "arch/arm64/virt_layout.h"
#include "core/machine.h"
#include "devices/platform/pl011.h"
#include "guest_programs/hello_program.h"

using namespace kestrel;
namespace L = kestrel::arm64::layout;

TEST(Vm, HelloProgramPrintsAndPowersOff) {
  auto machine = Machine::create({.ram_size = 16ull << 20, .cpus = 1});
  ASSERT_TRUE(machine) << machine.error().message;
  Machine& m = **machine;

  std::string output;
  auto uart = std::make_unique<Pl011>([&](uint8_t c) { output.push_back(static_cast<char>(c)); },
                                      [&](bool level) { m.set_spi(L::kUartSpi, level); });
  ASSERT_TRUE(m.attach(Gpa(L::kUartBase), L::kUartSize, std::move(uart)));

  const uint64_t entry = m.memory().base().value;
  ASSERT_TRUE(m.memory().write(Gpa(entry), hello_program()));

  auto stop = m.run(entry, 0);
  ASSERT_TRUE(stop) << stop.error().message;
  EXPECT_EQ(stop->reason, hv::StopReason::kPowerOff);
  EXPECT_EQ(output, "hello from inside the Kestrel VM!\n");
}

TEST(Vm, CanCreateVmAgainAfterDestroy) {
  for (int i = 0; i < 2; ++i) {
    auto machine = Machine::create({.ram_size = 16ull << 20, .cpus = 1});
    ASSERT_TRUE(machine) << machine.error().message;
  }
}

TEST(Vm, SecondaryCpusCreatedAndStopped) {
  // Four vCPUs are created; only CPU 0 runs the program. The others wait for
  // PSCI CPU_ON and must shut down cleanly when CPU 0 powers off.
  auto machine = Machine::create({.ram_size = 16ull << 20, .cpus = 4});
  ASSERT_TRUE(machine) << machine.error().message;
  Machine& m = **machine;
  std::string output;
  auto uart = std::make_unique<Pl011>([&](uint8_t c) { output.push_back(static_cast<char>(c)); },
                                      [&](bool level) { m.set_spi(L::kUartSpi, level); });
  ASSERT_TRUE(m.attach(Gpa(L::kUartBase), L::kUartSize, std::move(uart)));
  const uint64_t entry = m.memory().base().value;
  ASSERT_TRUE(m.memory().write(Gpa(entry), hello_program()));
  auto stop = m.run(entry, 0);
  ASSERT_TRUE(stop) << stop.error().message;
  EXPECT_EQ(stop->reason, hv::StopReason::kPowerOff);
  EXPECT_EQ(output, "hello from inside the Kestrel VM!\n");
}
