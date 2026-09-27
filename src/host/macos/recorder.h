#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "core/error.h"
#include "host/display.h"

namespace kestrel {

// Records the game to a video file: frames as the guest shows them and the sound
// it plays, encoded by the Mac's hardware H.264 encoder (AVFoundation). All calls are
// thread-safe; frames and audio may arrive from different threads.
class ScreenRecorder {
 public:
  static Result<std::unique_ptr<ScreenRecorder>> start(const std::string& path, uint32_t width, uint32_t height);
  virtual ~ScreenRecorder() = default;

  virtual void video(const Frame& frame) = 0;
  // Interleaved signed 16-bit PCM, as the game plays it.
  virtual void audio(std::span<const uint8_t> pcm, uint32_t rate, uint32_t channels) = 0;
  // Finishes the file; `done(ok)` runs on another thread once it's written.
  virtual void finish(std::function<void(bool ok)> done) = 0;
  virtual double seconds() const = 0;  // recorded so far
};

}  // namespace kestrel
