#include "android/cuttlefish_sensors.h"

#include <cstring>
#include <format>

#include "core/log.h"

namespace kestrel {
namespace {

// Sensor IDs (android-cuttlefish common/libs/sensors/sensors.h, same as goldfish).
constexpr int kAcceleration = 0, kGyroscope = 1, kMagnetic = 2, kTemperature = 4,
              kProximity = 5, kLight = 6, kPressure = 7, kHumidity = 8;
constexpr uint32_t kCmdUpdateHal = 2;
constexpr uint32_t kPhoneSensors = (1u << kAcceleration) | (1u << kGyroscope) |
                                   (1u << kMagnetic) | (1u << kTemperature) |
                                   (1u << kProximity) | (1u << kLight) | (1u << kPressure) |
                                   (1u << kHumidity);
constexpr size_t kMaxMessage = 64 * 1024;

}  // namespace

CuttlefishSensors::CuttlefishSensors(VirtioConsole& control, VirtioConsole& data)
    : control_(control), data_(data) {
  reporter_ = std::thread([this] { report_loop(); });
}

CuttlefishSensors::~CuttlefishSensors() {
  quit_ = true;
  cv_.notify_all();
  reporter_.join();
}

void CuttlefishSensors::set_gravity(float x, float y, float z) {
  std::lock_guard lock(mutex_);
  gravity_[0] = x;
  gravity_[1] = y;
  gravity_[2] = z;
}

void CuttlefishSensors::on_control_bytes(std::span<const uint8_t> bytes) {
  std::vector<std::pair<uint32_t, std::string>> messages;
  {
    std::lock_guard lock(mutex_);
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    while (buffer_.size() >= 8) {
      uint32_t word, size;
      std::memcpy(&word, buffer_.data(), 4);
      std::memcpy(&size, buffer_.data() + 4, 4);
      if (size > kMaxMessage) {  // garbage: resynchronize by dropping everything
        buffer_.clear();
        break;
      }
      if (buffer_.size() < 8 + size) break;
      messages.emplace_back(word & 0x7fffffff,
                            std::string(buffer_.begin() + 8, buffer_.begin() + 8 + size));
      buffer_.erase(buffer_.begin(), buffer_.begin() + 8 + size);
    }
  }
  for (auto& [cmd, payload] : messages) handle_message(cmd, payload);
}

void CuttlefishSensors::handle_message(uint32_t /*command*/, std::string_view payload) {
  if (payload.starts_with("list-sensors")) {
    send(control_, std::format("{}\n", kPhoneSensors));
    active_ = true;
    cv_.notify_all();
  }
}

void CuttlefishSensors::send(VirtioConsole& channel, std::string_view payload) {
  std::vector<uint8_t> msg(8 + payload.size());
  const uint32_t word = kCmdUpdateHal | (1u << 31);  // a response
  const auto size = static_cast<uint32_t>(payload.size());
  std::memcpy(msg.data(), &word, 4);
  std::memcpy(msg.data() + 4, &size, 4);
  std::memcpy(msg.data() + 8, payload.data(), payload.size());
  channel.send(msg);
}

void CuttlefishSensors::report_loop() {
  std::mutex wait_mutex;
  while (!quit_) {
    {
      std::unique_lock lock(wait_mutex);
      cv_.wait_for(lock, std::chrono::seconds(1), [&] { return quit_.load(); });
    }
    if (quit_ || !active_) continue;
    float g[3];
    {
      std::lock_guard lock(mutex_);
      std::memcpy(g, gravity_, sizeof(g));
    }
    // Continuous sensors, one message each (sensors_hal_proxy.cpp UpdateSensorsHal).
    send(data_, std::format("acceleration:{}:{}:{}\n", g[0], g[1], g[2]));
    send(data_, "gyroscope:0:0:0\n");
    send(data_, "magnetic:0:5.9:-48.4\n");
    send(data_, "light:400\n");
    send(data_, "pressure:1013.25\n");
  }
}

}  // namespace kestrel
