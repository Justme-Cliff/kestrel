#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "devices/virtio/virtio_console.h"

namespace kestrel {

// Host side of Cuttlefish's sensors HAL (the guest's sensors-service.multihal).
// Protocol from android-cuttlefish host/commands/sensors_simulator:
//  - control channel (/dev/hvc18): the guest sends "list-sensors"; we answer with
//    the bitmask of sensors the host provides.
//  - data channel (/dev/hvc19): once a second we report "<name>:<values>\n" for
//    continuous sensors.
// Messages are transport::RawMessage: le32 (command | is_response << 31),
// le32 payload size, payload.
class CuttlefishSensors {
 public:
  CuttlefishSensors(VirtioConsole& control, VirtioConsole& data);
  ~CuttlefishSensors();

  // Guest bytes arriving on the control channel.
  void on_control_bytes(std::span<const uint8_t> bytes);

  // Accelerometer vector (m/s^2) in device coordinates; sets the screen orientation.
  void set_gravity(float x, float y, float z);

 private:
  void handle_message(uint32_t command, std::string_view payload);
  void send(VirtioConsole& channel, std::string_view payload);
  void report_loop();

  VirtioConsole& control_;
  VirtioConsole& data_;
  std::mutex mutex_;
  std::vector<uint8_t> buffer_;
  float gravity_[3] = {0.0f, 9.81f, 0.0f};  // upright portrait
  std::atomic<bool> active_{false};
  std::atomic<bool> quit_{false};
  std::condition_variable cv_;
  std::thread reporter_;
};

}  // namespace kestrel
