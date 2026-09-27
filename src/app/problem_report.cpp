#include "app/problem_report.h"

#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

extern char** environ;

namespace kestrel {

namespace fs = std::filesystem;

namespace {

std::string sysctl_string(const char* name) {
  char buf[256] = {};
  size_t len = sizeof(buf) - 1;
  if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return "?";
  return buf;
}

uint64_t sysctl_u64(const char* name) {
  uint64_t v = 0;
  size_t len = sizeof(v);
  if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
  return v;
}

// Copies at most the last `max_bytes` of `from` (logs can be large; the end matters).
void copy_tail(const fs::path& from, const fs::path& to, std::uintmax_t max_bytes) {
  std::error_code ec;
  const auto size = fs::file_size(from, ec);
  if (ec) return;
  std::ifstream in(from, std::ios::binary);
  if (size > max_bytes) in.seekg(static_cast<std::streamoff>(size - max_bytes));
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  out << in.rdbuf();
}

int run(const std::vector<std::string>& argv) {
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid;
  if (posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return -1;
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

Result<std::string> write_problem_report(const std::string& logs_dir, const std::string& support_dir,
                                         const std::string& android_dir, const std::string& version) {
  const std::time_t now = std::time(nullptr);
  char stamp[64];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d at %H.%M.%S", std::localtime(&now));
  const char* home = std::getenv("HOME");
  if (!home) return make_error("no home folder");
  const fs::path dir = fs::temp_directory_path() / (std::string("Kestrel Report ") + stamp);
  std::error_code ec;
  fs::create_directories(dir / "profiles", ec);
  if (ec) return make_error("couldn't create " + dir.string());

  copy_tail(fs::path(logs_dir) / "kestrel.log", dir / "kestrel.log", 20u << 20);
  copy_tail(fs::path(logs_dir) / "console.log", dir / "console.log", 10u << 20);
  copy_tail(fs::path(logs_dir) / "kestrel.log.prev", dir / "kestrel.prev.log", 5u << 20);
  if (!android_dir.empty()) {
    copy_tail(fs::path(android_dir) / "instance" / "logcat.txt", dir / "logcat.txt", 10u << 20);
    copy_tail(fs::path(android_dir) / "instance" / "logcat.prev.txt", dir / "logcat.prev.txt", 10u << 20);
  }
  // The newest crash and freeze details: macOS crash reports, GPU stall samples, and
  // notes about apps that closed unexpectedly inside Android.
  auto newest = [&](const fs::path& in, const std::string& prefix, size_t count) {
    std::vector<fs::directory_entry> found;
    for (const auto& e : fs::directory_iterator(in, ec)) {
      if (e.path().filename().string().starts_with(prefix)) found.push_back(e);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
      std::error_code e1, e2;
      return a.last_write_time(e1) > b.last_write_time(e2);
    });
    if (found.size() > count) found.resize(count);
    return found;
  };
  for (const auto& e : newest(fs::path(home) / "Library/Logs/DiagnosticReports", "kestrel-", 3)) {
    fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
  }
  for (const char* prefix : {"gpu-stall-", "app-closed-", "last-crash", "perf"}) {
    for (const auto& e : newest(logs_dir, prefix, 5)) {
      fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    }
  }
  fs::copy_file(fs::path(support_dir) / "settings.json", dir / "settings.json", fs::copy_options::overwrite_existing, ec);
  for (const auto& e : fs::directory_iterator(fs::path(support_dir) / "profiles", ec)) {
    if (e.path().extension() == ".json") fs::copy_file(e.path(), dir / "profiles" / e.path().filename(), ec);
  }
  {
    std::ofstream info(dir / "info.txt");
    info << "Kestrel " << version << "\n"
         << "Mac: " << sysctl_string("hw.model") << ", " << sysctl_u64("hw.memsize") / (1024 * 1024) << " MB RAM, "
         << sysctl_u64("hw.ncpu") << " cores\n"
         << "macOS " << sysctl_string("kern.osproductversion") << " (" << sysctl_string("kern.osversion") << ")\n";
    std::ifstream build(fs::path(android_dir) / "BUILD_ID");
    if (std::string id; build && std::getline(build, id)) info << "Android build " << id << "\n";
  }

  const std::string zip = std::string(home) + "/Desktop/Kestrel Report " + stamp + ".zip";
  if (run({"/usr/bin/ditto", "-c", "-k", "--keepParent", dir.string(), zip}) != 0) {
    return make_error("couldn't write " + zip);
  }
  fs::remove_all(dir, ec);
  run({"/usr/bin/open", "-R", zip});  // show it in Finder
  return zip;
}

}  // namespace kestrel
