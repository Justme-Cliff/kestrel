#pragma once

#include <deque>
#include <functional>
#include <span>
#include <string>

namespace kestrel {

// Watches Android's log (logcat as it streams from the guest) for apps that died:
// a crash, or Android killing them for memory. Keeps the recent lines so the moments
// before are saved with the notice. Not thread-safe: feed from one thread.
class AppDeathWatcher {
 public:
  struct Death {
    std::string package;
    std::string reason;  // "crashed", "was closed to free memory", or ActivityManager's words
    std::string context;  // the log lines before and including the death
  };
  explicit AppDeathWatcher(std::function<void(const Death&)> on_death) : on_death_(std::move(on_death)) {}

  void feed(std::span<const uint8_t> bytes);

  static constexpr size_t kContextLines = 400;

 private:
  void line(const std::string& l);

  std::function<void(const Death&)> on_death_;
  std::string partial_;
  std::deque<std::string> recent_;
  std::string last_crash_package_;  // from "Process: <pkg>" after FATAL EXCEPTION / Fatal signal
};

}  // namespace kestrel
