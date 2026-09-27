#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "devices/virtio/virtio_device.h"
#include "host/audio.h"

namespace kestrel {

// virtio-snd (virtio 1.2 §5.14), laid out like Cuttlefish's card: stream 0 is
// stereo playback, stream 1 capture from the Mac's microphone (silence if there
// is no microphone or the user declines access).
// Playback buffers complete as the host audio device consumes them, which keeps
// the guest's audio clock in step with real time.
class VirtioSnd final : public VirtioDevice {
 public:
  VirtioSnd(std::unique_ptr<AudioOutput> output, std::unique_ptr<AudioInput> input = nullptr);
  ~VirtioSnd() override;

  uint32_t device_id() const override { return virtio_id::kSound; }
  std::string_view name() const override { return "virtio-snd"; }
  uint64_t features() const override { return 0; }
  unsigned num_queues() const override { return 4; }  // control, event, tx, rx
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void queue_notify(unsigned queue) override;
  void reset() override;

 private:
  enum class State { kIdle, kSet, kPrepared, kRunning };
  struct Buffer {
    Virtqueue::Chain chain;
    uint64_t end = 0;     // playback: position in the output timeline when this buffer ends
    bool queued = false;  // playback: samples copied to the host already
  };
  struct Stream {
    bool output;
    State state = State::kIdle;
    uint32_t channels = 2, rate = 48000;
    std::deque<Buffer> buffers;
    std::chrono::steady_clock::time_point started;
    uint64_t delivered = 0;  // capture: bytes of silence delivered since start
    uint64_t queued_bytes = 0;  // playback: bytes handed to the host since start
    uint64_t played_base = 0;   // playback: host played_bytes() at start
    int peak = 0;               // playback: loudest sample since start (diagnostics)
  };

  std::vector<uint8_t> control(std::span<const uint8_t> req);
  void pace();
  void complete(unsigned queue, std::vector<Buffer>& done);

  std::unique_ptr<AudioOutput> output_;
  std::unique_ptr<AudioInput> input_;
  std::mutex mutex_;
  Stream streams_[2] = {{.output = true}, {.output = false}};
  std::atomic<bool> quit_{false};
  std::condition_variable cv_;
  std::thread pacer_;
};

}  // namespace kestrel
