#pragma once

#include <cstdint>

// Guest physical memory map (docs/DESIGN.md §5.1). Modeled on QEMU's `virt` board;
// the PCI ECAM address matches crosvm so Android sees the "10000.pci" boot device.
namespace kestrel::arm64::layout {

constexpr uint64_t kPciEcamBase = 0x0001'0000;
constexpr uint64_t kPciEcamSize = 0x0010'0000;  // bus 0 only

constexpr uint64_t kGicDistBase = 0x0800'0000;
constexpr uint64_t kGicRedistBase = 0x0C00'0000;
constexpr uint64_t kGicRedistLimit = 0x1000'0000;  // Apple reserves 32 MB for redistributors
constexpr uint64_t kUartBase = 0x0900'0000;
constexpr uint64_t kUartSize = 0x1000;
constexpr uint32_t kUartSpi = 1;
constexpr uint64_t kRtcBase = 0x0901'0000;
constexpr uint64_t kRtcSize = 0x1000;
constexpr uint32_t kRtcSpi = 2;
constexpr uint64_t kVirtioMmioBase = 0x0A00'0000;
constexpr uint64_t kVirtioMmioSize = 0x200;
constexpr unsigned kVirtioMmioSlots = 32;
constexpr uint32_t kVirtioFirstSpi = 16;  // slot N uses SPI 16 + N
constexpr uint64_t kPciMmioBase = 0x1000'0000;
constexpr uint64_t kPciMmioSize = 0x3000'0000;
// 64-bit prefetchable window for large BARs (virtio-gpu host-visible memory).
constexpr uint64_t kPciMmio64Base = 0x8'0000'0000;  // 32 GB
constexpr uint64_t kPciMmio64Size = 0x4'0000'0000;  // 16 GB
constexpr uint32_t kPciFirstSpi = 32;  // PCI slot N uses SPI 32 + N (INTA)
constexpr uint64_t kRamBase = 0x4000'0000;

// Timer PPIs as written in the device tree (INTID - 16).
constexpr uint32_t kTimerSecurePpi = 13;
constexpr uint32_t kTimerNonSecurePpi = 14;
constexpr uint32_t kTimerVirtualPpi = 11;
constexpr uint32_t kTimerHypPpi = 10;

}  // namespace kestrel::arm64::layout
