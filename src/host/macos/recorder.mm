#include "host/macos/recorder.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

#include <cstring>
#include <mutex>

#include "core/log.h"

namespace kestrel {
namespace {

class MacRecorder final : public ScreenRecorder {
 public:
  MacRecorder(AVAssetWriter* writer, AVAssetWriterInput* video, AVAssetWriterInputPixelBufferAdaptor* adaptor,
              AVAssetWriterInput* audio, uint32_t w, uint32_t h)
      : writer_(writer), video_(video), adaptor_(adaptor), audio_(audio), width_(w), height_(h) {}

  void video(const Frame& f) override {
    std::lock_guard lock(mutex_);
    if (finished_ || f.width != width_ || f.height != height_) return;
    const double now = CACurrentMediaTime();
    if (!started_) {
      [writer_ startSessionAtSourceTime:kCMTimeZero];
      started_ = true;
      t0_ = now;
    }
    if (!video_.readyForMoreMediaData) return;  // encoder busy: drop this frame
    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor_.pixelBufferPool, &pb) != kCVReturnSuccess) return;
    CVPixelBufferLockBaseAddress(pb, 0);
    auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    const size_t dst_stride = CVPixelBufferGetBytesPerRow(pb);
    for (uint32_t y = 0; y < f.height; ++y) {
      const uint8_t* src = f.pixels.data() + size_t(y) * f.stride;
      uint8_t* row = dst + y * dst_stride;
      if (f.bgra) {
        std::memcpy(row, src, size_t(f.width) * 4);
      } else {  // RGBA -> BGRA
        for (uint32_t x = 0; x < f.width; ++x) {
          row[x * 4 + 0] = src[x * 4 + 2];
          row[x * 4 + 1] = src[x * 4 + 1];
          row[x * 4 + 2] = src[x * 4 + 0];
          row[x * 4 + 3] = 255;
        }
      }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    const CMTime t = CMTimeMakeWithSeconds(now - t0_, 60000);
    if (CMTimeCompare(t, last_video_) > 0) {  // strictly increasing timestamps
      [adaptor_ appendPixelBuffer:pb withPresentationTime:t];
      last_video_ = t;
    }
    CVPixelBufferRelease(pb);
  }

  void audio(std::span<const uint8_t> pcm, uint32_t rate, uint32_t channels) override {
    std::lock_guard lock(mutex_);
    if (finished_ || !started_ || pcm.empty() || channels == 0 || rate == 0) return;
    const size_t frame_bytes = 2 * channels;
    const CMItemCount frames = static_cast<CMItemCount>(pcm.size() / frame_bytes);
    if (frames == 0) return;
    // The audio clock is the sample count, anchored to when sound first arrived, so
    // it never drifts against itself.
    if (audio_frames_ == 0 || rate != audio_rate_) {
      audio_rate_ = rate;
      audio_base_ = CACurrentMediaTime() - t0_;
      audio_frames_ = 0;
    }
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate = rate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = static_cast<UInt32>(frame_bytes);
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = static_cast<UInt32>(frame_bytes);
    asbd.mChannelsPerFrame = channels;
    asbd.mBitsPerChannel = 16;
    CMAudioFormatDescriptionRef fmt = nullptr;
    if (CMAudioFormatDescriptionCreate(nullptr, &asbd, 0, nullptr, 0, nullptr, nullptr, &fmt) != noErr) return;
    CMBlockBufferRef block = nullptr;
    const size_t bytes = size_t(frames) * frame_bytes;
    if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes, nullptr, nullptr, 0, bytes, 0, &block) == noErr) {
      CMBlockBufferReplaceDataBytes(pcm.data(), block, 0, bytes);
      const CMTime pts = CMTimeAdd(CMTimeMakeWithSeconds(audio_base_, 48000), CMTimeMake(audio_frames_, static_cast<int32_t>(rate)));
      CMSampleBufferRef sample = nullptr;
      if (CMAudioSampleBufferCreateReadyWithPacketDescriptions(nullptr, block, fmt, frames, pts, nullptr, &sample) ==
          noErr) {
        if (audio_.readyForMoreMediaData) [audio_ appendSampleBuffer:sample];
        CFRelease(sample);
      }
      CFRelease(block);
    }
    CFRelease(fmt);
    audio_frames_ += frames;
  }

  void finish(std::function<void(bool)> done) override {
    std::lock_guard lock(mutex_);
    if (finished_) return;
    finished_ = true;
    if (!started_) {  // nothing was recorded
      [writer_ cancelWriting];
      if (done) done(false);
      return;
    }
    [video_ markAsFinished];
    [audio_ markAsFinished];
    AVAssetWriter* writer = writer_;
    auto shared = std::make_shared<std::function<void(bool)>>(std::move(done));
    [writer_ finishWritingWithCompletionHandler:^{
      const bool ok = writer.status == AVAssetWriterStatusCompleted;
      if (!ok) log(LogLevel::kWarn, "recording: {}", writer.error ? writer.error.localizedDescription.UTF8String : "failed");
      if (*shared) (*shared)(ok);
    }];
  }

  double seconds() const override {
    std::lock_guard lock(mutex_);
    return started_ ? CACurrentMediaTime() - t0_ : 0;
  }

 private:
  mutable std::mutex mutex_;
  AVAssetWriter* writer_;
  AVAssetWriterInput* video_;
  AVAssetWriterInputPixelBufferAdaptor* adaptor_;
  AVAssetWriterInput* audio_;
  uint32_t width_, height_;
  bool started_ = false, finished_ = false;
  double t0_ = 0;
  CMTime last_video_ = kCMTimeNegativeInfinity;
  uint32_t audio_rate_ = 0;
  double audio_base_ = 0;
  int64_t audio_frames_ = 0;
};

}  // namespace

Result<std::unique_ptr<ScreenRecorder>> ScreenRecorder::start(const std::string& path, uint32_t width, uint32_t height) {
  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  [NSFileManager.defaultManager removeItemAtURL:url error:nil];
  NSError* error = nil;
  AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
  if (!writer) return make_error(std::string("can't create the video file: ") + error.localizedDescription.UTF8String);
  // H.264 plays everywhere (phones, WhatsApp, Discord); ~12 Mbit/s keeps 720p60 sharp.
  NSDictionary* video_settings = @{
    AVVideoCodecKey : AVVideoCodecTypeH264,
    AVVideoWidthKey : @(width),
    AVVideoHeightKey : @(height),
    AVVideoCompressionPropertiesKey : @{
      AVVideoAverageBitRateKey : @(width * height * 13),
      AVVideoExpectedSourceFrameRateKey : @60,
      AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
    },
  };
  AVAssetWriterInput* video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                 outputSettings:video_settings];
  video.expectsMediaDataInRealTime = YES;
  AVAssetWriterInputPixelBufferAdaptor* adaptor = [AVAssetWriterInputPixelBufferAdaptor
      assetWriterInputPixelBufferAdaptorWithAssetWriterInput:video
                                 sourcePixelBufferAttributes:@{
                                   (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                                   (id)kCVPixelBufferWidthKey : @(width),
                                   (id)kCVPixelBufferHeightKey : @(height),
                                 }];
  AudioChannelLayout layout{};
  layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
  NSDictionary* audio_settings = @{
    AVFormatIDKey : @(kAudioFormatMPEG4AAC),
    AVSampleRateKey : @48000,
    AVNumberOfChannelsKey : @2,
    AVEncoderBitRateKey : @192000,
    AVChannelLayoutKey : [NSData dataWithBytes:&layout length:sizeof(layout)],
  };
  AVAssetWriterInput* audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                 outputSettings:audio_settings];
  audio.expectsMediaDataInRealTime = YES;
  if (![writer canAddInput:video] || ![writer canAddInput:audio]) return make_error("the video encoder isn't available");
  [writer addInput:video];
  [writer addInput:audio];
  if (![writer startWriting]) {
    return make_error(std::string("can't start recording: ") + (writer.error ? writer.error.localizedDescription.UTF8String : "?"));
  }
  return std::unique_ptr<ScreenRecorder>(new MacRecorder(writer, video, adaptor, audio, width, height));
}

}  // namespace kestrel
