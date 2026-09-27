#include "core/log.h"

#include <atomic>
#include <cstdio>

namespace kestrel {
namespace {
std::atomic<LogLevel> g_level{LogLevel::kInfo};
}

void set_log_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level; }

void log_message(LogLevel level, std::string_view message) {
  static constexpr const char* kNames[] = {"debug", "info", "warn", "error"};
  // "\r\n" so logs stay readable while the terminal is in raw mode.
  std::fprintf(stderr, "[kestrel %s] %.*s\r\n", kNames[static_cast<int>(level)],
               static_cast<int>(message.size()), message.data());
}

}  // namespace kestrel
