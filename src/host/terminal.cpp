#include "host/terminal.h"

#include <poll.h>
#include <unistd.h>

namespace kestrel {
namespace {
constexpr uint8_t kCtrlRightBracket = 0x1d;
}

Terminal::Terminal(InputFn on_input, QuitFn on_quit)
    : on_input_(std::move(on_input)), on_quit_(std::move(on_quit)) {
  if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_) == 0) {
    termios raw = saved_;
    cfmakeraw(&raw);
    raw.c_oflag |= OPOST | ONLCR;  // keep "\n" -> "\r\n" for host log lines
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    raw_ = true;
  }
  reader_ = std::thread([this] { reader_loop(); });
}

Terminal::~Terminal() {
  stop_ = true;
  reader_.join();
  if (raw_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
}

void Terminal::write_output(uint8_t byte) { (void)::write(STDOUT_FILENO, &byte, 1); }

void Terminal::reader_loop() {
  pollfd pfd{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
  while (!stop_) {
    if (poll(&pfd, 1, 100) <= 0) continue;
    uint8_t buf[64];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
      if (n == 0) return;  // stdin closed (e.g. running from a script)
      continue;
    }
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == kCtrlRightBracket) {
        on_quit_();
        return;
      }
      on_input_(buf[i]);
    }
  }
}

}  // namespace kestrel
