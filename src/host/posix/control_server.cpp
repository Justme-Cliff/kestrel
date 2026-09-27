#include "host/control_server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include "core/log.h"

namespace kestrel {

ControlServer::ControlServer(std::string path, Handler handler)
    : path_(std::move(path)), handler_(std::move(handler)) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path_.size() >= sizeof(addr.sun_path)) {
    log(LogLevel::kError, "control socket path too long");
    return;
  }
  std::strcpy(addr.sun_path, path_.c_str());
  ::unlink(path_.c_str());
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  const mode_t old_mask = ::umask(0177);  // socket file mode 0600: owner only
  const bool bound = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::umask(old_mask);
  if (!bound || ::listen(listen_fd_, 4) != 0) {
    log(LogLevel::kError, "cannot listen on control socket {}", path_);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return;
  }
  if (::pipe(wake_pipe_) != 0) wake_pipe_[0] = wake_pipe_[1] = -1;
  thread_ = std::thread([this] { loop(); });
}

ControlServer::~ControlServer() {
  quit_ = true;
  if (wake_pipe_[1] >= 0) (void)!::write(wake_pipe_[1], "q", 1);
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    ::unlink(path_.c_str());
  }
  if (wake_pipe_[0] >= 0) ::close(wake_pipe_[0]);
  if (wake_pipe_[1] >= 0) ::close(wake_pipe_[1]);
}

void ControlServer::loop() {
  while (!quit_) {
    pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
    if (::poll(fds, 2, -1) <= 0 || (fds[1].revents & POLLIN)) continue;
    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) continue;
    const int one = 1;
    ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    // One client at a time; commands are quick.
    std::string buffer;
    char chunk[512];
    while (!quit_) {
      const ssize_t n = ::read(client, chunk, sizeof(chunk));
      if (n <= 0) break;
      buffer.append(chunk, static_cast<size_t>(n));
      if (buffer.size() > 64 * 1024) break;  // not a well-behaved client
      size_t nl;
      while ((nl = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string err = handler_(line);
        // Handlers answer "" (ok), "ok <result>" (a measurement) or an error message.
        const std::string reply = err.empty()              ? "ok\n"
                                  : err.starts_with("ok ") ? err + "\n"
                                                           : "error: " + err + "\n";
        (void)!::write(client, reply.data(), reply.size());
      }
    }
    ::close(client);
  }
}

}  // namespace kestrel
