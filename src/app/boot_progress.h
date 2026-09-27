#pragma once

#include <functional>
#include <string>

namespace kestrel {

// Turns the guest's console output into a boot progress bar: known milestones in
// the kernel and init log lines move it forward (it never goes back).
class BootProgress {
 public:
  using ProgressFn = std::function<void(double fraction, const std::string& text)>;
  explicit BootProgress(ProgressFn progress);

  void feed(uint8_t byte);  // one console byte
  void finish();            // Android is ready: 100%

 private:
  void line(const std::string& text);
  void report(double fraction, const char* text);

  ProgressFn progress_;
  std::string line_;
  double fraction_ = 0;
};

}  // namespace kestrel
