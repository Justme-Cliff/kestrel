#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/error.h"

namespace kestrel {

// A minimal adb client (system/core/adb/protocol.txt) so the app can configure
// Android and install APKs without the `adb` tool. Talks to adbd through our
// vsock bridge on 127.0.0.1:<port>. One connection per call; blocking.
class AdbClient {
 public:
  explicit AdbClient(uint16_t port) : port_(port) {}

  // Runs a shell command and returns its output.
  Result<std::string> shell(const std::string& command);

  // Installs an APK ("cmd package install -S"). `progress` gets 0..1.
  Result<void> install(const std::string& apk_path,
                       const std::function<void(double)>& progress = nullptr);

  // Copies a host file to the device (adb "sync" protocol), e.g. /sdcard/Download/x.
  Result<void> push(const std::string& local_path, const std::string& remote_path);

  // Retries until adbd answers and sys.boot_completed is 1, or `timeout_s` passes.
  bool wait_for_boot(int timeout_s);
  // Longest silence a command may have before it's treated as lost (installs and
  // pushes get at least 15 minutes); 0 means wait forever (the command-line tool).
  void set_timeout(int seconds) { timeout_s_ = seconds; }

 private:
  uint16_t port_;
  int timeout_s_ = 30;
};

}  // namespace kestrel
