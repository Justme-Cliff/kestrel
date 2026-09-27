#include "devices/platform/pl011.h"

#include <gtest/gtest.h>

#include <string>

using namespace kestrel;

namespace {
struct Harness {
  std::string out;
  bool irq = false;
  int irq_changes = 0;
  Pl011 uart{[this](uint8_t c) { out.push_back(static_cast<char>(c)); },
             [this](bool l) {
               irq = l;
               ++irq_changes;
             }};
};
}  // namespace

TEST(Pl011, TransmitsBytes) {
  Harness h;
  for (char c : std::string("hi")) h.uart.mmio_write(0x000, 4, c);
  EXPECT_EQ(h.out, "hi");
}

TEST(Pl011, IdentifiesAsPl011) {
  Harness h;
  EXPECT_EQ(h.uart.mmio_read(0xfe0, 4), 0x11u);
  EXPECT_EQ(h.uart.mmio_read(0xfe8, 4), 0x14u);
  EXPECT_EQ(h.uart.mmio_read(0xffc, 4), 0xb1u);
}

TEST(Pl011, ReceiveRaisesMaskedInterrupt) {
  Harness h;
  EXPECT_EQ(h.uart.mmio_read(0x018, 4) & (1u << 4), 1u << 4);  // RXFE
  h.uart.push_input('x');
  EXPECT_FALSE(h.irq);  // masked
  h.uart.mmio_write(0x038, 4, Pl011::kIntRx);
  EXPECT_TRUE(h.irq);
  EXPECT_EQ(h.uart.mmio_read(0x018, 4) & (1u << 4), 0u);
  EXPECT_EQ(h.uart.mmio_read(0x000, 4), static_cast<uint64_t>('x'));
  EXPECT_FALSE(h.irq);  // FIFO drained
}

TEST(Pl011, TxInterruptClearedByIcr) {
  Harness h;
  h.uart.mmio_write(0x038, 4, Pl011::kIntTx);
  h.uart.mmio_write(0x000, 4, 'a');
  EXPECT_TRUE(h.irq);
  EXPECT_EQ(h.uart.mmio_read(0x040, 4), Pl011::kIntTx);
  h.uart.mmio_write(0x044, 4, Pl011::kIntTx);
  EXPECT_FALSE(h.irq);
}
