#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace kestrel {

// A local control channel for scripts and tests: a Unix socket that accepts
// one command per line and answers "ok" or "error: ...". Commands are handled
// by `handler`, which returns an empty string on success or an error message.
// Only the current user can connect (the socket is created with mode 0600).
class ControlServer {
 public:
  using Handler = std::function<std::string(const std::string& line)>;

  ControlServer(std::string path, Handler handler);
  ~ControlServer();
  bool ok() const { return listen_fd_ >= 0; }

 private:
  void loop();

  std::string path_;
  Handler handler_;
  int listen_fd_ = -1;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> quit_{false};
  std::thread thread_;
};

}  // namespace kestrel
