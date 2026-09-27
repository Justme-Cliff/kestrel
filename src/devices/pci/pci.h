#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "core/mmio_device.h"

namespace kestrel {

// A PCI function with a standard type-0 configuration header (PCI Local Bus 3.0 §6).
// Handles command/status, memory BARs (with sizing) and the capability list;
// subclasses implement BAR accesses and fill in capabilities.
class PciFunction {
 public:
  struct Ids {
    uint16_t vendor, device;
    uint8_t revision;
    uint32_t class_code;  // 24-bit: base class, subclass, prog-if
    uint16_t subsystem_vendor, subsystem;
  };
  static constexpr unsigned kNumBars = 6;

  explicit PciFunction(const Ids& ids);
  virtual ~PciFunction() = default;
  virtual std::string_view name() const = 0;

  uint32_t config_read(uint16_t offset, unsigned size);
  void config_write(uint16_t offset, unsigned size, uint32_t value);

  // If `addr` falls inside an enabled memory BAR, returns (bar, offset).
  bool decode(uint64_t addr, unsigned size, unsigned& bar, uint64_t& offset);
  virtual uint64_t bar_read(unsigned bar, uint64_t offset, unsigned size) = 0;
  virtual void bar_write(unsigned bar, uint64_t offset, unsigned size, uint64_t value) = 0;

  // Guest address of a memory BAR, if the guest assigned it and enabled decoding.
  std::optional<uint64_t> bar_base(unsigned index);

  // Wired by the host bridge to this function's INTx line.
  void set_intx_sink(std::function<void(bool)> sink) { intx_sink_ = std::move(sink); }

 protected:
  // A 64-bit memory BAR occupying `index` and `index + 1`.
  void add_bar64(unsigned index, uint64_t size, bool prefetchable = false);
  // Appends a capability; `body` excludes the 2-byte ID/next header. Returns its offset.
  uint8_t add_capability(uint8_t id, std::vector<uint8_t> body);
  void set_intx(bool level);

 private:
  uint64_t bar_address(unsigned index) const;

  std::mutex mutex_;
  std::array<uint8_t, 256> config_{};
  std::array<uint64_t, kNumBars> bar_size_{};  // 0 = unused; set on the low half of 64-bit BARs
  std::array<uint32_t, kNumBars> bar_flags_{};
  uint8_t next_cap_ = 0x40;
  uint8_t last_cap_ = 0;
  std::function<void(bool)> intx_sink_;
  bool intx_level_ = false;
};

// Generic ECAM host bridge (Linux "pci-host-ecam-generic"), bus 0 only.
// Exposes two MMIO regions: the ECAM configuration space and the BAR window.
class PciHost {
 public:
  static constexpr unsigned kMaxDevices = 32;

  // `window_base` is the guest address of the BAR window; `irq(slot, level)`
  // drives the INTx line for a device slot.
  PciHost(uint64_t window_base, uint64_t window64_base,
          std::function<void(unsigned slot, bool level)> irq);

  // Adds a function at the next free slot (starting at 1) and returns the slot.
  unsigned add(std::unique_ptr<PciFunction> function);
  unsigned device_count() const { return static_cast<unsigned>(functions_.size()); }
  std::vector<unsigned> slots() const;

  MmioDevice& ecam() { return ecam_; }
  MmioDevice& window() { return window_; }
  MmioDevice& window64() { return window64_; }

 private:
  class Ecam final : public MmioDevice {
   public:
    explicit Ecam(PciHost& host) : host_(host) {}
    std::string_view name() const override { return "pci-ecam"; }
    uint64_t mmio_read(uint64_t offset, unsigned size) override;
    void mmio_write(uint64_t offset, unsigned size, uint64_t value) override;

   private:
    PciHost& host_;
  };
  class Window final : public MmioDevice {
   public:
    Window(PciHost& host, uint64_t base) : host_(host), base_(base) {}
    std::string_view name() const override { return "pci-mmio"; }
    uint64_t mmio_read(uint64_t offset, unsigned size) override;
    void mmio_write(uint64_t offset, unsigned size, uint64_t value) override;

   private:
    PciHost& host_;
    uint64_t base_;
  };

  PciFunction* at_slot(unsigned slot);

  std::function<void(unsigned, bool)> irq_;
  std::vector<std::pair<unsigned, std::unique_ptr<PciFunction>>> functions_;
  Ecam ecam_;
  Window window_;
  Window window64_;
};

}  // namespace kestrel
