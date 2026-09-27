#include "app/app_watch.h"

namespace kestrel {

void AppDeathWatcher::feed(std::span<const uint8_t> bytes) {
  for (uint8_t b : bytes) {
    if (b == '\n') {
      line(partial_);
      partial_.clear();
    } else if (b != '\r' && partial_.size() < 4096) {
      partial_.push_back(static_cast<char>(b));
    }
  }
}

void AppDeathWatcher::line(const std::string& l) {
  recent_.push_back(l);
  if (recent_.size() > kContextLines) recent_.pop_front();
  auto context = [&] {
    std::string c;
    for (const auto& r : recent_) c += r + "\n";
    return c;
  };
  // Java crash: "E AndroidRuntime: Process: com.dts.freefireth, PID: 1234"
  if (auto at = l.find("AndroidRuntime: Process: "); at != std::string::npos) {
    const size_t start = at + 25;
    last_crash_package_ = l.substr(start, l.find(',', start) - start);
  }
  // Native crash: ">>> com.dts.freefireth <<<" in the tombstone header.
  if (auto at = l.find(">>> "); at != std::string::npos && l.find(" <<<", at) != std::string::npos) {
    const size_t start = at + 4;
    last_crash_package_ = l.substr(start, l.find(" <<<", start) - start);
  }
  // "I ActivityManager: Process com.dts.freefireth (pid 1234) has died: fg  TOP"
  const std::string died = "ActivityManager: Process ";
  if (auto at = l.find(died); at != std::string::npos && l.find(" has died") != std::string::npos) {
    const size_t start = at + died.size();
    const std::string package = l.substr(start, l.find(' ', start) - start);
    // Only apps the user was looking at: background deaths are routine.
    const size_t state = l.find("has died: ");
    const std::string how = state == std::string::npos ? "" : l.substr(state + 10);
    if (!how.starts_with("fg") && !how.starts_with("vis")) return;
    std::string reason = "closed unexpectedly";
    if (package == last_crash_package_) reason = "crashed";
    for (const auto& r : recent_) {
      if (r.find("lowmemorykiller") != std::string::npos && r.find(package) != std::string::npos &&
          r.find("Kill") != std::string::npos) {
        reason = "was closed by Android to free memory";
      }
    }
    if (on_death_) on_death_({package, reason, context()});
    last_crash_package_.clear();
  }
}

}  // namespace kestrel
