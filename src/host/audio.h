#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace kestrel {

// Host audio playback (per-OS implementation). Interleaved signed 16-bit PCM.
class AudioOutput {
 public:
  static std::unique_ptr<AudioOutput> create();  // nullptr if no audio device
  virtual ~AudioOutput() = default;

  virtual bool start(uint32_t rate, uint32_t channels) = 0;
  virtual void stop() = 0;
  // Queues samples; returns how many bytes fit (the rest must be offered again).
  virtual size_t write(std::span<const uint8_t> pcm) = 0;
  // Bytes the hardware has played since start() (drives virtio-snd pacing).
  virtual uint64_t played_bytes() const = 0;
  virtual size_t buffered_bytes() const = 0;
};

// Host microphone capture (per-OS). Interleaved signed 16-bit PCM. macOS asks the
// user for permission the first time start() is called.
class AudioInput {
 public:
  static std::unique_ptr<AudioInput> create();
  virtual ~AudioInput() = default;
  virtual bool start(uint32_t rate, uint32_t channels) = 0;
  virtual void stop() = 0;
  // Takes up to out.size() bytes of recorded audio; returns how many.
  virtual size_t read(std::span<uint8_t> out) = 0;
};

}  // namespace kestrel
