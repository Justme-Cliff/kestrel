#include "app/instances.h"

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/clonefile.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

extern char** environ;

namespace kestrel {
namespace fs = std::filesystem;

std::string current_instance() {
  const char* v = std::getenv("KESTREL_INSTANCE");
  return v && valid_instance_name(v) ? v : "";
}

bool valid_instance_name(const std::string& name) {
  if (name.empty() || name.size() > 24 || name == "soaktest") return false;
  return std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

std::string instance_dir(const std::string& support_dir, const std::string& name) {
  return support_dir + (name.empty() ? "/android" : "/android-" + name);
}

std::vector<std::string> list_instances(const std::string& support_dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(support_dir, ec)) {
    const std::string f = e.path().filename().string();
    if (e.is_directory() && f.starts_with("android-") && valid_instance_name(f.substr(8)) &&
        fs::exists(e.path() / "boot.img")) {
      names.push_back(f.substr(8));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<void> create_instance(const std::string& support_dir, const std::string& name) {
  if (!valid_instance_name(name)) return make_error("Use letters, numbers, - or _ (up to 24).");
  const fs::path main = instance_dir(support_dir, ""), dir = instance_dir(support_dir, name);
  if (fs::exists(dir)) return make_error("An instance called “" + name + "” already exists.");
  if (!fs::exists(main / "boot.img")) return make_error("Start Kestrel once first: the main Android isn't set up yet.");
  std::error_code ec;
  fs::create_directories(dir / "instance", ec);
  if (ec) return make_error("couldn't create " + dir.string());
  // Images (read-only) and the expanded system partition: APFS clones, no copy.
  auto clone = [&](const fs::path& from, const fs::path& to) -> Result<void> {
    if (clonefile(from.c_str(), to.c_str(), 0) == 0) return {};
    fs::copy_file(from, to, ec);  // another file system: a real copy
    if (ec) return make_error("couldn't copy " + from.filename().string());
    return {};
  };
  for (const auto& e : fs::directory_iterator(main, ec)) {
    if (!e.is_regular_file()) continue;
    if (auto r = clone(e.path(), dir / e.path().filename()); !r) return r;
  }
  if (fs::exists(main / "instance" / "super.raw")) {
    if (auto r = clone(main / "instance" / "super.raw", dir / "instance" / "super.raw"); !r) return r;
  }
  return {};  // userdata is created empty on first boot: a fresh Android
}

Result<void> launch_instance(const std::string& name) {
  char self[4096];
  uint32_t size = sizeof(self);
  if (_NSGetExecutablePath(self, &size) != 0) return make_error("can't find Kestrel");
  std::vector<std::string> env;
  for (char** e = environ; *e; ++e) {
    if (!std::string_view(*e).starts_with("KESTREL_INSTANCE=")) env.push_back(*e);
  }
  if (!name.empty()) env.push_back("KESTREL_INSTANCE=" + name);
  std::vector<char*> envp;
  for (auto& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);
  char* argv[] = {self, nullptr};
  pid_t pid;
  if (posix_spawn(&pid, self, nullptr, nullptr, argv, envp.data()) != 0) return make_error("couldn't start Kestrel");
  return {};
}

}  // namespace kestrel
