#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace kestrel {

// Frame pacing from the times frames reach the screen: average FPS, the "1% low"
// (the FPS the slowest 1% of frames run at, which is what a player feels as stutter),
// the worst frame, and hitches (frames that took more than twice the usual time).
// record() is called from the GPU thread, take() from the UI; both are cheap.
class FrameStats {
 public:
  using Clock = std::chrono::steady_clock;
  struct Window {
    unsigned frames = 0;
    double seconds = 0, fps = 0, low1_fps = 0, worst_ms = 0;
    unsigned hitches = 0;
  };

  void record(Clock::time_point t = Clock::now()) {
    std::lock_guard lock(mutex_);
    if (last_ != Clock::time_point{}) {
      const double ms = std::chrono::duration<double, std::milli>(t - last_).count();
      if (ms < 1000) intervals_.push_back(ms);  // longer = paused, not a frame
    }
    last_ = t;
  }

  // Stats since the previous take().
  Window take(Clock::time_point now = Clock::now()) {
    std::vector<double> d;
    Clock::time_point since;
    {
      std::lock_guard lock(mutex_);
      d.swap(intervals_);
      since = window_start_;
      window_start_ = now;
    }
    Window w;
    w.seconds = since == Clock::time_point{} ? 0 : std::chrono::duration<double>(now - since).count();
    w.frames = static_cast<unsigned>(d.size());
    if (d.empty() || w.seconds <= 0) return w;
    w.fps = d.size() / w.seconds;
    std::sort(d.begin(), d.end());
    const double median = d[d.size() / 2];
    const double p99 = d[std::min(d.size() - 1, d.size() * 99 / 100)];
    w.low1_fps = p99 > 0 ? 1000.0 / p99 : 0;
    w.worst_ms = d.back();
    w.hitches = static_cast<unsigned>(std::count_if(d.begin(), d.end(), [&](double x) { return x > 2 * median; }));
    return w;
  }

 private:
  std::mutex mutex_;
  std::vector<double> intervals_;
  Clock::time_point last_{}, window_start_{};
};

}  // namespace kestrel
