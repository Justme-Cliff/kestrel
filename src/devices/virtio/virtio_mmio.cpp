#include "devices/virtio/virtio_mmio.h"

#include <cstring>

namespace kestrel {
namespace {

enum Reg : uint64_t {
  kMagic = 0x000,
  kVersion = 0x004,
  kDeviceId = 0x008,
  kVendorId = 0x00c,
  kDeviceFeatures = 0x010,
  kDeviceFeaturesSel = 0x014,
  kDriverFeatures = 0x020,
  kDriverFeaturesSel = 0x024,
  kQueueSel = 0x030,
  kQueueNumMax = 0x034,
  kQueueNum = 0x038,
  kQueueReady = 0x044,
  kQueueNotify = 0x050,
  kInterruptStatus = 0x060,
  kInterruptAck = 0x064,
  kStatus = 0x070,
  kQueueDescLow = 0x080,
  kQueueDescHigh = 0x084,
  kQueueDriverLow = 0x090,
  kQueueDriverHigh = 0x094,
  kQueueDeviceLow = 0x0a0,
  kQueueDeviceHigh = 0x0a4,
  kConfigGeneration = 0x0fc,
  kConfig = 0x100,
};

constexpr uint32_t kMagicValue = 0x74726976;  // "virt"
constexpr uint32_t kVendor = 0x4c54534b;      // "KSTL"

using Ring = VirtioTransport::Ring;

}  // namespace

uint64_t VirtioMmio::mmio_read(uint64_t offset, unsigned size) {
  if (offset >= kConfig) {
    uint64_t v = 0;
    if (size <= 8) {
      uint8_t buf[8] = {};
      transport_.device().read_config(offset - kConfig, std::span<uint8_t>(buf, size));
      std::memcpy(&v, buf, size);
    }
    return v;
  }
  switch (offset) {
    case kMagic: return kMagicValue;
    case kVersion: return 2;
    case kDeviceId: return transport_.device().device_id();
    case kVendorId: return kVendor;
    case kDeviceFeatures: return transport_.device_features_word(device_features_sel_);
    case kQueueNumMax: return transport_.queue_exists() ? transport_.queue_max_size() : 0;
    case kQueueReady: return transport_.queue_ready();
    case kInterruptStatus: return transport_.interrupt_status();
    case kStatus: return transport_.status();
    case kConfigGeneration: return 0;
    default: return 0;
  }
}

void VirtioMmio::mmio_write(uint64_t offset, unsigned size, uint64_t value) {
  if (offset >= kConfig) {
    if (size <= 8) {
      uint8_t buf[8];
      std::memcpy(buf, &value, 8);
      transport_.device().write_config(offset - kConfig, std::span<const uint8_t>(buf, size));
    }
    return;
  }
  const auto v = static_cast<uint32_t>(value);
  switch (offset) {
    case kDeviceFeaturesSel: device_features_sel_ = v; break;
    case kDriverFeaturesSel: driver_features_sel_ = v; break;
    case kDriverFeatures: transport_.set_driver_features_word(driver_features_sel_, v); break;
    case kQueueSel: transport_.select_queue(v); break;
    case kQueueNum: transport_.set_queue_size(static_cast<uint16_t>(std::min<uint32_t>(v, 0xffff))); break;
    case kQueueDescLow: transport_.set_queue_addr_low(Ring::kDesc, v); break;
    case kQueueDescHigh: transport_.set_queue_addr_high(Ring::kDesc, v); break;
    case kQueueDriverLow: transport_.set_queue_addr_low(Ring::kDriver, v); break;
    case kQueueDriverHigh: transport_.set_queue_addr_high(Ring::kDriver, v); break;
    case kQueueDeviceLow: transport_.set_queue_addr_low(Ring::kDevice, v); break;
    case kQueueDeviceHigh: transport_.set_queue_addr_high(Ring::kDevice, v); break;
    case kQueueReady: transport_.set_queue_ready(v & 1); break;
    case kQueueNotify: transport_.notify(v); break;
    case kInterruptAck: transport_.ack_interrupt(v); break;
    case kStatus: transport_.set_status(v); break;
    default: break;
  }
}

}  // namespace kestrel
