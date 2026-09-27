#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "host/display.h"

namespace kestrel {

// Headless display: keeps the latest frame and writes it as a binary PPM image
// at most once per `interval` (for tests and debugging without a window).
class FrameFile final : public DisplaySink {
 public:
  explicit FrameFile(std::string path, std::chrono::milliseconds interval = std::chrono::seconds(1));
  ~FrameFile() override;
  void present(const Frame& frame) override;

 private:
  void write_locked();

  std::string path_;
  std::chrono::milliseconds interval_;
  std::mutex mutex_;
  std::vector<uint8_t> rgb_;
  uint32_t width_ = 0, height_ = 0;
  bool dirty_ = false;
  std::chrono::steady_clock::time_point last_write_{};
};

}  // namespace kestrel
