#include "arch/arm64/psci.h"

#include <gtest/gtest.h>

using namespace kestrel::arm64;

TEST(Psci, Version) {
  auto r = handle_psci(psci::kVersion, 0);
  EXPECT_EQ(r.action, PsciAction::kReturn);
  EXPECT_EQ(r.value, psci::kVersion1_0);
}

TEST(Psci, SystemOffAndReset) {
  EXPECT_EQ(handle_psci(psci::kSystemOff, 0).action, PsciAction::kSystemOff);
  EXPECT_EQ(handle_psci(psci::kSystemReset, 0).action, PsciAction::kSystemReset);
}

TEST(Psci, Features) {
  EXPECT_EQ(handle_psci(psci::kFeatures, psci::kCpuOn64).value, psci::kSuccess);
  EXPECT_EQ(handle_psci(psci::kFeatures, 0x12345678).value, psci::kNotSupported);
}

TEST(Psci, UnknownFunction) {
  EXPECT_EQ(handle_psci(0x80000000, 0).value, psci::kNotSupported);
}
