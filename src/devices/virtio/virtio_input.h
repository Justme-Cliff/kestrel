#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "devices/virtio/virtio_device.h"

namespace kestrel {

// Linux input event codes (include/uapi/linux/input-event-codes.h) we use.
namespace input {
constexpr uint16_t kEvSyn = 0x00, kEvKey = 0x01, kEvAbs = 0x03;
constexpr uint16_t kSynReport = 0;
constexpr uint16_t kBtnTouch = 0x14a;
constexpr uint16_t kAbsX = 0x00, kAbsY = 0x01;
constexpr uint16_t kAbsMtSlot = 0x2f, kAbsMtPositionX = 0x35, kAbsMtPositionY = 0x36,
                   kAbsMtTrackingId = 0x39;
constexpr uint32_t kPropDirect = 0x01;
}  // namespace input

struct InputEvent {
  uint16_t type, code;
  int32_t value;
};

// virtio-input (virtio 1.2 §5.8). The device's identity (name, supported events,
// axis ranges) is answered through the config space; events go to the guest on
// queue 0. Keymapper (Phase 6) and the window's mouse feed events in.
class VirtioInput final : public VirtioDevice {
 public:
  struct AbsInfo {
    int32_t min, max, fuzz, flat, res;
  };
  struct Identity {
    std::string name;
    uint16_t product;
    std::vector<uint32_t> props;
    std::map<uint16_t, std::vector<uint16_t>> events;  // event type -> codes
    std::map<uint16_t, AbsInfo> abs;
  };

  static Identity touchscreen(uint32_t width, uint32_t height, unsigned slots = 21);
  static Identity keyboard();

  explicit VirtioInput(Identity identity);

  uint32_t device_id() const override { return virtio_id::kInput; }
  std::string_view name() const override { return "virtio-input"; }
  uint64_t features() const override { return 0; }
  unsigned num_queues() const override { return 2; }  // event, status
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void write_config(uint64_t offset, std::span<const uint8_t> in) override;
  void queue_notify(unsigned queue) override;
  void reset() override;

  // Thread-safe. Callers end each batch with a SYN_REPORT.
  void send(std::span<const InputEvent> events);

  // Shrinks a backlog the guest hasn't read yet (it stalled for a moment) without
  // losing state: drops touch frames that only move a finger which moves again later
  // in the backlog. Presses, lifts and keys are always kept; a dropped press or lift
  // would leave a finger stuck on the screen. The first frame may be half sent to
  // the guest already and is never touched.
  static void compact_backlog(std::deque<InputEvent>& backlog);

 private:
  void refresh_config_locked();
  void flush();

  Identity id_;
  std::mutex mutex_;
  uint8_t select_ = 0, subsel_ = 0;
  std::vector<uint8_t> payload_;  // answer for the current select/subsel
  std::deque<InputEvent> pending_;
};

}  // namespace kestrel
