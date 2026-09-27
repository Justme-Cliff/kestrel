#include "boot/virt_dtb.h"

#include <format>

#include "arch/arm64/virt_layout.h"
#include "boot/fdt_writer.h"

namespace kestrel {
namespace {

constexpr uint32_t kPhandleGic = 1;
constexpr uint32_t kPhandleClock = 2;
constexpr uint32_t kGicSpi = 0;
constexpr uint32_t kGicPpi = 1;
constexpr uint32_t kIrqLevelHigh = 4;

uint32_t hi(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
uint32_t lo(uint64_t v) { return static_cast<uint32_t>(v); }

}  // namespace

Result<std::vector<uint8_t>> build_virt_dtb(const VirtBoardConfig& cfg) {
  namespace L = arm64::layout;
  FdtWriter w;
  w.begin_node("");
  w.prop_string("compatible", "linux,dummy-virt");
  w.prop_string("model", "Kestrel virtual machine");
  w.prop_u32("#address-cells", 2);
  w.prop_u32("#size-cells", 2);
  w.prop_u32("interrupt-parent", kPhandleGic);

  const std::string uart_path = std::format("/pl011@{:x}", L::kUartBase);
  w.begin_node("chosen");
  w.prop_string("bootargs", cfg.cmdline);
  w.prop_string("stdout-path", uart_path);
  if (cfg.initrd) {
    w.prop_u64("linux,initrd-start", cfg.initrd->first);
    w.prop_u64("linux,initrd-end", cfg.initrd->second);
  }
  w.end_node();

  w.begin_node(std::format("memory@{:x}", cfg.ram_base));
  w.prop_string("device_type", "memory");
  w.prop_cells("reg", {hi(cfg.ram_base), lo(cfg.ram_base), hi(cfg.ram_size), lo(cfg.ram_size)});
  w.end_node();

  w.begin_node("cpus");
  w.prop_u32("#address-cells", 1);
  w.prop_u32("#size-cells", 0);
  for (unsigned i = 0; i < cfg.cpus; ++i) {
    w.begin_node(std::format("cpu@{}", i));
    w.prop_string("device_type", "cpu");
    w.prop_string("compatible", "arm,arm-v8");
    w.prop_u32("reg", i);  // must match MPIDR_EL1 affinity
    w.prop_string("enable-method", "psci");
    w.end_node();
  }
  w.end_node();

  w.begin_node("psci");
  w.prop_strings("compatible", {"arm,psci-1.0", "arm,psci-0.2"});
  w.prop_string("method", "hvc");
  w.end_node();

  w.begin_node("timer");
  w.prop_string("compatible", "arm,armv8-timer");
  w.prop_cells("interrupts", {kGicPpi, L::kTimerSecurePpi, kIrqLevelHigh,     //
                              kGicPpi, L::kTimerNonSecurePpi, kIrqLevelHigh,  //
                              kGicPpi, L::kTimerVirtualPpi, kIrqLevelHigh,    //
                              kGicPpi, L::kTimerHypPpi, kIrqLevelHigh});
  w.prop_empty("always-on");
  w.end_node();

  w.begin_node(std::format("intc@{:x}", cfg.gic_dist_base));
  w.prop_string("compatible", "arm,gic-v3");
  w.prop_u32("#interrupt-cells", 3);
  w.prop_empty("interrupt-controller");
  w.prop_u32("#redistributor-regions", 1);
  w.prop_cells("reg", {hi(cfg.gic_dist_base), lo(cfg.gic_dist_base), hi(cfg.gic_dist_size),
                       lo(cfg.gic_dist_size), hi(cfg.gic_redist_base), lo(cfg.gic_redist_base),
                       hi(cfg.gic_redist_size), lo(cfg.gic_redist_size)});
  w.prop_u32("phandle", kPhandleGic);
  w.end_node();

  w.begin_node("apb-pclk");
  w.prop_string("compatible", "fixed-clock");
  w.prop_u32("#clock-cells", 0);
  w.prop_u32("clock-frequency", 24'000'000);
  w.prop_string("clock-output-names", "clk24mhz");
  w.prop_u32("phandle", kPhandleClock);
  w.end_node();

  w.begin_node(uart_path.substr(1));
  w.prop_strings("compatible", {"arm,pl011", "arm,primecell"});
  w.prop_cells("reg", {hi(L::kUartBase), lo(L::kUartBase), hi(L::kUartSize), lo(L::kUartSize)});
  w.prop_cells("interrupts", {kGicSpi, L::kUartSpi, kIrqLevelHigh});
  w.prop_cells("clocks", {kPhandleClock, kPhandleClock});
  w.prop_strings("clock-names", {"uartclk", "apb_pclk"});
  w.end_node();


  w.begin_node(std::format("pl031@{:x}", L::kRtcBase));
  w.prop_strings("compatible", {"arm,pl031", "arm,primecell"});
  w.prop_cells("reg", {hi(L::kRtcBase), lo(L::kRtcBase), hi(L::kRtcSize), lo(L::kRtcSize)});
  w.prop_cells("interrupts", {kGicSpi, L::kRtcSpi, kIrqLevelHigh});
  w.prop_u32("clocks", kPhandleClock);
  w.prop_string("clock-names", "apb_pclk");
  w.end_node();

  for (unsigned i = 0; i < cfg.virtio_slots; ++i) {
    const uint64_t base = L::kVirtioMmioBase + i * L::kVirtioMmioSize;
    w.begin_node(std::format("virtio_mmio@{:x}", base));
    w.prop_string("compatible", "virtio,mmio");
    w.prop_cells("reg", {hi(base), lo(base), hi(L::kVirtioMmioSize), lo(L::kVirtioMmioSize)});
    w.prop_cells("interrupts", {kGicSpi, L::kVirtioFirstSpi + i, kIrqLevelHigh});
    w.prop_empty("dma-coherent");
    w.end_node();
  }

  if (!cfg.pci_slots.empty()) {
    constexpr uint32_t kPciMem32 = 0x02000000;  // ranges: 32-bit non-prefetchable memory
    w.begin_node(std::format("pci@{:x}", L::kPciEcamBase));
    w.prop_string("compatible", "pci-host-ecam-generic");
    w.prop_string("device_type", "pci");
    w.prop_u32("#address-cells", 3);
    w.prop_u32("#size-cells", 2);
    w.prop_u32("#interrupt-cells", 1);
    w.prop_cells("bus-range", {0, 0});
    w.prop_cells("reg", {hi(L::kPciEcamBase), lo(L::kPciEcamBase), hi(L::kPciEcamSize),
                         lo(L::kPciEcamSize)});
    constexpr uint32_t kPciMem64Pref = 0x43000000;  // 64-bit prefetchable memory
    w.prop_cells("ranges", {kPciMem32, hi(L::kPciMmioBase), lo(L::kPciMmioBase),
                            hi(L::kPciMmioBase), lo(L::kPciMmioBase), hi(L::kPciMmioSize),
                            lo(L::kPciMmioSize),  //
                            kPciMem64Pref, hi(L::kPciMmio64Base), lo(L::kPciMmio64Base),
                            hi(L::kPciMmio64Base), lo(L::kPciMmio64Base), hi(L::kPciMmio64Size),
                            lo(L::kPciMmio64Size)});
    // INTA of device N -> SPI kPciFirstSpi + N. The GIC has no #address-cells, so
    // each entry is: pci addr (3), pin (1), gic phandle, gic interrupt specifier (3).
    std::vector<uint32_t> map;
    for (unsigned slot : cfg.pci_slots) {
      map.insert(map.end(), {slot << 11, 0, 0, 1, kPhandleGic, kGicSpi, L::kPciFirstSpi + slot,
                             kIrqLevelHigh});
    }
    w.prop_cells("interrupt-map", map);
    w.prop_cells("interrupt-map-mask", {0xf800, 0, 0, 7});
    w.prop_empty("dma-coherent");
    w.end_node();
  }

  w.end_node();
  return w.finish();
}

}  // namespace kestrel
