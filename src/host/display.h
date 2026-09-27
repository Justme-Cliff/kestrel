#pragma once

#include <cstdint>
#include <span>

namespace kestrel {

// One finished frame from the guest's scanout. Pixels are 32-bit, `stride` bytes per row.
struct Frame {
  uint32_t width, height, stride;
  bool bgra;  // true: B,G,R,A byte order; false: R,G,B,A
  std::span<const uint8_t> pixels;
};

// Where the virtual GPU sends finished frames: a window, or a file in headless mode.
// present() is called on a vCPU thread and must copy what it needs and return fast.
class DisplaySink {
 public:
  virtual ~DisplaySink() = default;
  virtual void present(const Frame& frame) = 0;
  // A frame was shown directly by the GPU (no pixels to hand over), for FPS counts.
  virtual void frame_shown() {}
};

}  // namespace kestrel
