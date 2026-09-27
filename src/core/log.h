#pragma once

#include <format>
#include <string_view>

namespace kestrel {

enum class LogLevel { kDebug, kInfo, kWarn, kError };

void set_log_level(LogLevel level);
LogLevel log_level();
void log_message(LogLevel level, std::string_view message);

template <typename... Args>
void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
  if (level < log_level()) return;
  log_message(level, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace kestrel
