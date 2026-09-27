#include "app/boot_progress.h"

#include <iterator>

namespace kestrel {

namespace {
struct Milestone {
  const char* marker;  // substring of a console line
  double fraction;
  const char* text;
};
// In boot order. Fractions follow measured timings of a typical boot.
constexpr Milestone kMilestones[] = {
    {"Booting Linux", 0.05, "Starting Linux…"},
    {"Run /init", 0.15, "Starting Android…"},
    {"init second stage started", 0.22, "Starting Android…"},
    {"starting service 'servicemanager'", 0.3, "Starting system services…"},
    {"starting service 'zygote'", 0.45, "Starting the Android framework…"},
    {"starting service 'surfaceflinger'", 0.55, "Starting the display…"},
    {"starting service 'bootanim'", 0.7, "Almost ready…"},
    {"sys.boot_completed=1", 0.95, "Ready"},
};
}  // namespace

BootProgress::BootProgress(ProgressFn progress) : progress_(std::move(progress)) {}

void BootProgress::feed(uint8_t byte) {
  if (fraction_ >= 1) return;
  if (byte == '\n') {
    line(line_);
    line_.clear();
  } else if (line_.size() < 512) {  // console lines are short; cap guest-controlled input
    line_.push_back(static_cast<char>(byte));
  }
}

void BootProgress::line(const std::string& text) {
  for (const auto& m : kMilestones) {
    if (m.fraction > fraction_ && text.find(m.marker) != std::string::npos) report(m.fraction, m.text);
  }
}

void BootProgress::report(double fraction, const char* text) {
  fraction_ = fraction;
  if (progress_) progress_(fraction, text);
}

void BootProgress::finish() {
  if (fraction_ < 1) report(1.0, "Ready");
}

}  // namespace kestrel
