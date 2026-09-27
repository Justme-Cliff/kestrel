// AudioOutput on CoreAudio: a default-output AudioUnit pulling from a lock-free
// single-producer/single-consumer ring buffer.

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "host/audio.h"

namespace kestrel {
namespace {

class CoreAudioOutput final : public AudioOutput {
 public:
  ~CoreAudioOutput() override { stop(); }

  bool start(uint32_t rate, uint32_t channels) override {
    stop();
    frame_bytes_ = channels * 2;
    // ~200 ms of buffering: enough to absorb guest scheduling jitter.
    ring_.assign(size_t(rate) * frame_bytes_ / 5, 0);
    head_ = tail_ = 0;
    played_ = 0;

    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &unit_) != noErr) return false;
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel = 16;
    fmt.mChannelsPerFrame = channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = frame_bytes_;
    fmt.mBytesPerPacket = frame_bytes_;
    AURenderCallbackStruct cb{&CoreAudioOutput::render, this};
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                             sizeof(fmt)) != noErr ||
        AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                             sizeof(cb)) != noErr ||
        AudioUnitInitialize(unit_) != noErr || AudioOutputUnitStart(unit_) != noErr) {
      log(LogLevel::kWarn, "audio: couldn't start CoreAudio output");
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
      return false;
    }
    return true;
  }

  void stop() override {
    if (!unit_) return;
    AudioOutputUnitStop(unit_);
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
  }

  size_t write(std::span<const uint8_t> pcm) override {
    if (ring_.empty()) return 0;
    const size_t cap = ring_.size();
    const size_t used = head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire);
    const size_t n = std::min(pcm.size(), cap - used);
    size_t head = head_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n;) {
      const size_t at = head % cap;
      const size_t chunk = std::min(n - i, cap - at);
      std::memcpy(&ring_[at], pcm.data() + i, chunk);
      i += chunk;
      head += chunk;
    }
    head_.store(head, std::memory_order_release);
    return n;
  }

  uint64_t played_bytes() const override { return played_.load(std::memory_order_acquire); }
  size_t buffered_bytes() const override {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }

 private:
  // Real-time audio thread: no locks, no allocation. Plays silence on underrun but
  // still counts it as played, so the guest's clock keeps moving.
  static OSStatus render(void* self_ptr, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                         UInt32 frames, AudioBufferList* list) {
    auto* self = static_cast<CoreAudioOutput*>(self_ptr);
    auto* out = static_cast<uint8_t*>(list->mBuffers[0].mData);
    const size_t want = size_t(frames) * self->frame_bytes_;
    const size_t cap = self->ring_.size();
    size_t tail = self->tail_.load(std::memory_order_relaxed);
    const size_t avail = self->head_.load(std::memory_order_acquire) - tail;
    const size_t n = std::min(want, avail);
    for (size_t i = 0; i < n;) {
      const size_t at = tail % cap;
      const size_t chunk = std::min(n - i, cap - at);
      std::memcpy(out + i, &self->ring_[at], chunk);
      i += chunk;
      tail += chunk;
    }
    std::memset(out + n, 0, want - n);
    self->tail_.store(tail, std::memory_order_release);
    self->played_.fetch_add(want, std::memory_order_release);
    return noErr;
  }

  AudioComponentInstance unit_ = nullptr;
  uint32_t frame_bytes_ = 4;
  std::vector<uint8_t> ring_;
  std::atomic<size_t> head_{0}, tail_{0};
  std::atomic<uint64_t> played_{0};
};

}  // namespace

std::unique_ptr<AudioOutput> AudioOutput::create() { return std::make_unique<CoreAudioOutput>(); }

}  // namespace kestrel

// ---- microphone: an AudioQueue input feeding a mutex-guarded byte queue ----

#include <deque>
#include <mutex>

namespace kestrel {
namespace {

class CoreAudioInput final : public AudioInput {
 public:
  ~CoreAudioInput() override { stop(); }

  bool start(uint32_t rate, uint32_t channels) override {
    stop();
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel = 16;
    fmt.mChannelsPerFrame = channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = channels * 2;
    fmt.mBytesPerPacket = channels * 2;
    if (AudioQueueNewInput(&fmt, &CoreAudioInput::callback, this, nullptr, nullptr, 0, &queue_) != noErr) {
      queue_ = nullptr;
      return false;
    }
    const UInt32 bytes = rate * fmt.mBytesPerFrame / 50;  // 20 ms buffers
    for (int i = 0; i < 4; ++i) {
      AudioQueueBufferRef b;
      if (AudioQueueAllocateBuffer(queue_, bytes, &b) == noErr) AudioQueueEnqueueBuffer(queue_, b, 0, nullptr);
    }
    max_bytes_ = rate * fmt.mBytesPerFrame / 5;  // keep at most 200 ms
    if (AudioQueueStart(queue_, nullptr) != noErr) {
      stop();
      return false;
    }
    return true;
  }

  void stop() override {
    if (!queue_) return;
    AudioQueueStop(queue_, true);
    AudioQueueDispose(queue_, true);
    queue_ = nullptr;
    std::lock_guard lock(mutex_);
    data_.clear();
  }

  size_t read(std::span<uint8_t> out) override {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(out.size(), data_.size());
    std::copy_n(data_.begin(), n, out.begin());
    data_.erase(data_.begin(), data_.begin() + static_cast<long>(n));
    return n;
  }

 private:
  static void callback(void* self_ptr, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp*, UInt32,
                       const AudioStreamPacketDescription*) {
    auto* self = static_cast<CoreAudioInput*>(self_ptr);
    {
      std::lock_guard lock(self->mutex_);
      const auto* p = static_cast<const uint8_t*>(b->mAudioData);
      self->data_.insert(self->data_.end(), p, p + b->mAudioDataByteSize);
      if (self->data_.size() > self->max_bytes_) {  // guest not reading: drop the oldest
        self->data_.erase(self->data_.begin(), self->data_.begin() + static_cast<long>(self->data_.size() - self->max_bytes_));
      }
    }
    AudioQueueEnqueueBuffer(q, b, 0, nullptr);
  }

  AudioQueueRef queue_ = nullptr;
  std::mutex mutex_;
  std::deque<uint8_t> data_;
  size_t max_bytes_ = 0;
};

}  // namespace

std::unique_ptr<AudioInput> AudioInput::create() { return std::make_unique<CoreAudioInput>(); }

}  // namespace kestrel
