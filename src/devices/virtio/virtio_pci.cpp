#include "devices/virtio/virtio_pci.h"

#include <cstring>
#include <optional>

namespace kestrel {
namespace {

constexpr uint16_t kVendorRedHat = 0x1af4;  // the PCI vendor ID reserved for virtio
constexpr uint8_t kCapVendor = 0x09;

enum CfgType : uint8_t { kCommonCfg = 1, kNotifyCfg = 2, kIsrCfg = 3, kDeviceCfg = 4, kSharedMemoryCfg = 8 };
constexpr unsigned kShmBar = 2;

// struct virtio_pci_common_cfg (§4.1.4.3)
enum Common : uint64_t {
  kDeviceFeatureSelect = 0x00,
  kDeviceFeature = 0x04,
  kDriverFeatureSelect = 0x08,
  kDriverFeature = 0x0c,
  kConfigMsixVector = 0x10,
  kNumQueues = 0x12,
  kDeviceStatus = 0x14,
  kConfigGeneration = 0x15,
  kQueueSelect = 0x16,
  kQueueSize = 0x18,
  kQueueMsixVector = 0x1a,
  kQueueEnable = 0x1c,
  kQueueNotifyOff = 0x1e,
  kQueueDescLo = 0x20,
  kQueueDescHi = 0x24,
  kQueueDriverLo = 0x28,
  kQueueDriverHi = 0x2c,
  kQueueDeviceLo = 0x30,
  kQueueDeviceHi = 0x34,
  kCommonSize = 0x38,
};

constexpr uint16_t kNoVector = 0xffff;

uint32_t class_for(uint32_t virtio_id) {
  switch (virtio_id) {
    case virtio_id::kNet: return 0x020000;      // Ethernet controller
    case virtio_id::kBlock: return 0x010000;    // SCSI storage (what virtio-blk uses)
    case virtio_id::kConsole: return 0x078000;  // communication controller
    case virtio_id::kGpu: return 0x038000;      // display controller
    case virtio_id::kInput: return 0x090000;    // input device
    default: return 0xff0000;
  }
}

std::vector<uint8_t> virtio_cap(uint8_t cfg_type, uint32_t offset, uint32_t length,
                                std::optional<uint32_t> notify_multiplier = std::nullopt) {
  // struct virtio_pci_cap minus the 2-byte ID/next header (§4.1.4).
  std::vector<uint8_t> b(notify_multiplier ? 18 : 14, 0);
  b[0] = static_cast<uint8_t>(b.size() + 2);  // cap_len
  b[1] = cfg_type;
  b[2] = 0;  // BAR0
  std::memcpy(&b[6], &offset, 4);
  std::memcpy(&b[10], &length, 4);
  if (notify_multiplier) std::memcpy(&b[14], &*notify_multiplier, 4);
  return b;
}

using Ring = VirtioTransport::Ring;

}  // namespace

VirtioPci::VirtioPci(const GuestMemory& memory, std::unique_ptr<VirtioDevice> device)
    : PciFunction(Ids{.vendor = kVendorRedHat,
                      .device = static_cast<uint16_t>(0x1040 + device->device_id()),
                      .revision = 1,
                      .class_code = class_for(device->device_id()),
                      .subsystem_vendor = kVendorRedHat,
                      .subsystem = 0x40}),
      transport_(memory, std::move(device), [this](bool level) { set_intx(level); }) {
  add_bar64(0, kBarSize);
  add_capability(kCapVendor, virtio_cap(kCommonCfg, kCommonOffset, kCommonSize));
  add_capability(kCapVendor, virtio_cap(kIsrCfg, kIsrOffset, 4));
  add_capability(kCapVendor, virtio_cap(kDeviceCfg, kDeviceOffset, 0x1000));
  add_capability(kCapVendor, virtio_cap(kNotifyCfg, kNotifyOffset,
                                        transport_.num_queues() * kNotifyMultiplier,
                                        kNotifyMultiplier));
  if (const uint64_t shm = transport_.device().shared_memory_size()) {
    // struct virtio_pci_cap64 (§4.1.4.9): a BAR holding the device's shared memory.
    add_bar64(kShmBar, shm, /*prefetchable=*/true);
    std::vector<uint8_t> b(22, 0);
    b[0] = 24;  // cap_len
    b[1] = kSharedMemoryCfg;
    b[2] = kShmBar;
    b[3] = transport_.device().shared_memory_id();
    const uint32_t offset_lo = 0, length_lo = static_cast<uint32_t>(shm), offset_hi = 0,
                   length_hi = static_cast<uint32_t>(shm >> 32);
    std::memcpy(&b[6], &offset_lo, 4);
    std::memcpy(&b[10], &length_lo, 4);
    std::memcpy(&b[14], &offset_hi, 4);
    std::memcpy(&b[18], &length_hi, 4);
    add_capability(kCapVendor, b);
    transport_.device().bind_shared_memory([this] { return bar_base(kShmBar); });
  }
}

uint64_t VirtioPci::bar_read(unsigned bar, uint64_t offset, unsigned size) {
  if (bar != 0) return 0;
  if (offset < kIsrOffset) return common_read(offset, size);
  if (offset < kDeviceOffset) return offset == kIsrOffset ? transport_.read_and_clear_interrupt() : 0;
  if (offset < kNotifyOffset) {
    uint64_t v = 0;
    if (size <= 8) {
      uint8_t buf[8] = {};
      transport_.device().read_config(offset - kDeviceOffset, std::span<uint8_t>(buf, size));
      std::memcpy(&v, buf, size);
    }
    return v;
  }
  return 0;
}

void VirtioPci::bar_write(unsigned bar, uint64_t offset, unsigned size, uint64_t value) {
  if (bar != 0) return;
  if (offset < kIsrOffset) {
    common_write(offset, size, static_cast<uint32_t>(value));
  } else if (offset >= kDeviceOffset && offset < kNotifyOffset) {
    if (size <= 8) {
      uint8_t buf[8];
      std::memcpy(buf, &value, 8);
      transport_.device().write_config(offset - kDeviceOffset, std::span<const uint8_t>(buf, size));
    }
  } else if (offset >= kNotifyOffset) {
    transport_.notify(static_cast<uint32_t>((offset - kNotifyOffset) / kNotifyMultiplier));
  }
}

uint64_t VirtioPci::common_read(uint64_t offset, unsigned /*size*/) {
  switch (offset) {
    case kDeviceFeatureSelect: return device_features_sel_;
    case kDeviceFeature: return transport_.device_features_word(device_features_sel_);
    case kDriverFeatureSelect: return driver_features_sel_;
    case kConfigMsixVector: return kNoVector;
    case kNumQueues: return transport_.num_queues();
    case kDeviceStatus: return transport_.status();
    case kConfigGeneration: return 0;
    case kQueueSelect: return transport_.selected_queue();
    case kQueueSize: return transport_.queue_size();
    case kQueueMsixVector: return kNoVector;
    case kQueueEnable: return transport_.queue_ready();
    case kQueueNotifyOff: return transport_.selected_queue();  // notify at queue * multiplier
    case kQueueDescLo: return static_cast<uint32_t>(transport_.queue_addr(Ring::kDesc));
    case kQueueDescHi: return transport_.queue_addr(Ring::kDesc) >> 32;
    case kQueueDriverLo: return static_cast<uint32_t>(transport_.queue_addr(Ring::kDriver));
    case kQueueDriverHi: return transport_.queue_addr(Ring::kDriver) >> 32;
    case kQueueDeviceLo: return static_cast<uint32_t>(transport_.queue_addr(Ring::kDevice));
    case kQueueDeviceHi: return transport_.queue_addr(Ring::kDevice) >> 32;
    default: return 0;
  }
}

void VirtioPci::common_write(uint64_t offset, unsigned size, uint32_t v) {
  switch (offset) {
    case kDeviceFeatureSelect: device_features_sel_ = v; break;
    case kDriverFeatureSelect: driver_features_sel_ = v; break;
    case kDriverFeature: transport_.set_driver_features_word(driver_features_sel_, v); break;
    case kDeviceStatus: transport_.set_status(v & 0xff); break;
    case kQueueSelect: transport_.select_queue(v & 0xffff); break;
    case kQueueSize: transport_.set_queue_size(static_cast<uint16_t>(v)); break;
    case kQueueEnable: transport_.set_queue_ready(v & 1); break;
    case kQueueDescLo: transport_.set_queue_addr_low(Ring::kDesc, v); break;
    case kQueueDescHi: transport_.set_queue_addr_high(Ring::kDesc, v); break;
    case kQueueDriverLo: transport_.set_queue_addr_low(Ring::kDriver, v); break;
    case kQueueDriverHi: transport_.set_queue_addr_high(Ring::kDriver, v); break;
    case kQueueDeviceLo: transport_.set_queue_addr_low(Ring::kDevice, v); break;
    case kQueueDeviceHi: transport_.set_queue_addr_high(Ring::kDevice, v); break;
    default:
      (void)size;
      break;
  }
}

}  // namespace kestrel
