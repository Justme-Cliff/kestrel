#include "arch/arm64/esr.h"

#include <gtest/gtest.h>

using namespace kestrel::arm64;

namespace {
uint64_t data_abort_esr(bool isv, unsigned sas, bool sse, unsigned srt, bool sf, bool wnr) {
  return (0x24ull << 26) | (uint64_t(isv) << 24) | (uint64_t(sas) << 22) | (uint64_t(sse) << 21) |
         (uint64_t(srt) << 16) | (uint64_t(sf) << 15) | (uint64_t(wnr) << 6);
}
}  // namespace

TEST(Esr, DecodesByteStore) {
  auto a = decode_data_abort(data_abort_esr(true, 0, false, 3, false, true));
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->is_write);
  EXPECT_EQ(a->size_bytes, 1u);
  EXPECT_EQ(a->reg, 3u);
}

TEST(Esr, DecodesSignExtendedLoad) {
  auto a = decode_data_abort(data_abort_esr(true, 1, true, 30, true, false));
  ASSERT_TRUE(a);
  EXPECT_FALSE(a->is_write);
  EXPECT_EQ(a->size_bytes, 2u);
  EXPECT_TRUE(a->sign_extend);
  EXPECT_TRUE(a->reg_is_64bit);
  EXPECT_EQ(a->reg, 30u);
}

TEST(Esr, RejectsMissingSyndrome) {
  EXPECT_FALSE(decode_data_abort(data_abort_esr(false, 2, false, 0, false, false)));
}

TEST(Esr, RejectsOtherExceptionClasses) {
  EXPECT_FALSE(decode_data_abort(0x16ull << 26));
  EXPECT_EQ(exception_class(0x16ull << 26), 0x16);
}

TEST(Esr, DecodesSysReg) {
  // MRS x5, op0=3 op1=0 crn=0 crm=0 op2=5 (MPIDR_EL1)
  uint64_t esr = (0x18ull << 26) | (3ull << 20) | (5ull << 17) | (5ull << 5) | 1;
  auto s = decode_sysreg(esr);
  EXPECT_TRUE(s.is_read);
  EXPECT_EQ(s.reg, 5u);
  EXPECT_EQ(s.op0, 3u);
  EXPECT_EQ(s.op2, 5u);
}
