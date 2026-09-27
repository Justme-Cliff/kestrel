#include "host/frame_file.h"

#include <cstdio>
#include <format>

namespace kestrel {

FrameFile::FrameFile(std::string path, std::chrono::milliseconds interval)
    : path_(std::move(path)), interval_(interval) {}

FrameFile::~FrameFile() {
  std::lock_guard lock(mutex_);
  if (dirty_) write_locked();
}

void FrameFile::present(const Frame& f) {
  std::lock_guard lock(mutex_);
  width_ = f.width;
  height_ = f.height;
  rgb_.resize(size_t(f.width) * f.height * 3);
  for (uint32_t y = 0; y < f.height; ++y) {
    const uint8_t* src = f.pixels.data() + size_t(y) * f.stride;
    uint8_t* dst = rgb_.data() + size_t(y) * f.width * 3;
    for (uint32_t x = 0; x < f.width; ++x, src += 4, dst += 3) {
      dst[0] = f.bgra ? src[2] : src[0];
      dst[1] = src[1];
      dst[2] = f.bgra ? src[0] : src[2];
    }
  }
  dirty_ = true;
  if (std::chrono::steady_clock::now() - last_write_ >= interval_) write_locked();
}

void FrameFile::write_locked() {
  const std::string tmp = path_ + ".tmp";
  if (FILE* out = std::fopen(tmp.c_str(), "wb")) {
    const std::string header = std::format("P6\n{} {}\n255\n", width_, height_);
    std::fwrite(header.data(), 1, header.size(), out);
    std::fwrite(rgb_.data(), 1, rgb_.size(), out);
    std::fclose(out);
    std::rename(tmp.c_str(), path_.c_str());  // readers never see a half-written file
  }
  dirty_ = false;
  last_write_ = std::chrono::steady_clock::now();
}

}  // namespace kestrel
