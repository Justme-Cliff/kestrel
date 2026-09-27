#include "app/crash_marker.h"

#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace kestrel {
namespace {

char g_path[1024];

void write_str(int fd, const char* s) { (void)!::write(fd, s, std::strlen(s)); }

// Only async-signal-safe calls (open/write/close/raise); backtrace() is the one
// exception every crash reporter makes, and the process is dying anyway.
void on_fatal(int sig) {
  const int fd = ::open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    char msg[64] = "Kestrel stopped by signal ";
    char num[8];
    int n = sig, i = 0;
    do num[i++] = static_cast<char>('0' + n % 10); while ((n /= 10) && i < 7);
    size_t len = std::strlen(msg);
    while (i > 0) msg[len++] = num[--i];
    msg[len++] = '\n';
    msg[len] = 0;
    write_str(fd, msg);
    void* frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), fd);
    ::close(fd);
  }
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

}  // namespace

void install_crash_marker(const std::string& path) {
  std::strncpy(g_path, path.c_str(), sizeof(g_path) - 1);
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) std::signal(sig, on_fatal);
}

std::optional<std::string> take_crash_marker(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  std::stringstream text;
  text << in.rdbuf();
  in.close();
  std::error_code ec;
  std::filesystem::rename(path, path + ".seen", ec);
  return text.str();
}

}  // namespace kestrel
